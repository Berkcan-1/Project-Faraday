"""ARM FP oracle: Fraction (tam rasyonel) aritmetik + ARM FPRound/FPProcessNaNs kuralları.
C++ soft-float'tan BAĞIMSIZ yazıldı (bit işlemi yerine gerçek sayılar). RN modu CPython'un yerel IEEE float'larıyla çapraz doğrulanır (selfcheck)."""
from fractions import Fraction as Fr
import math, random, struct

IOC, DZC, OFC, UFC, IXC, IDC = 1, 2, 4, 8, 16, 128
FM = {'h': (5, 10), 's': (8, 23), 'd': (11, 52)}
RN, RP, RM, RZ, RA = 0, 1, 2, 3, 4

class Env:
    def __init__(self, rmode=RN, fz=False, dn=False): self.rmode, self.fz, self.dn, self.flags = rmode, fz, dn, 0
    @staticmethod
    def from_fpcr(v): return Env((v >> 22) & 3, bool((v >> 24) & 1), bool((v >> 25) & 1))

def P(f):
    E, F = FM[f]; bias = (1 << (E - 1)) - 1
    return E, F, bias, 1 - bias
def zero(f, s): E, F, _, _ = P(f); return s << (E + F)
def inf(f, s): E, F, _, _ = P(f); return (((1 << E) - 1) << F) | (s << (E + F))
def defnan(f): E, F, _, _ = P(f); return (((1 << E) - 1) << F) | (1 << (F - 1))
def maxnorm(f, s): E, F, _, _ = P(f); return (((1 << E) - 2) << F) | ((1 << F) - 1) | (s << (E + F))
def quiet(f, raw): E, F, _, _ = P(f); return raw | (1 << (F - 1))

def cls(f, bits, env):
    """-> (kind, sign, magnitude(Fraction), raw)"""
    E, F, bias, emin = P(f)
    sign = (bits >> (E + F)) & 1; ef = (bits >> F) & ((1 << E) - 1); frac = bits & ((1 << F) - 1)
    if ef == (1 << E) - 1:
        if frac == 0: return ('inf', sign, None, bits)
        return ('qnan' if (frac >> (F - 1)) & 1 else 'snan', sign, None, bits)
    if ef == 0:
        if frac == 0: return ('zero', sign, Fr(0), bits)
        if env.fz and f != 'h':
            env.flags |= IDC
            return ('zero', sign, Fr(0), bits)
        return ('norm', sign, Fr(frac, 1 << F) * Fr(2) ** emin, bits)
    return ('norm', sign, (1 + Fr(frac, 1 << F)) * Fr(2) ** (ef - bias), bits)

def floor_log2(x):
    e = x.numerator.bit_length() - x.denominator.bit_length()
    if Fr(2) ** e > x: e -= 1
    return e

def fp_round(f, sign, x, env):
    E, F, bias, emin = P(f)
    exponent = floor_log2(x)
    if env.fz and f != 'h' and exponent < emin:
        env.flags |= UFC
        return zero(f, sign)
    biased = max(exponent - emin + 1, 0)
    mant = x / Fr(2) ** exponent
    if biased == 0: mant = mant / Fr(2) ** (emin - exponent)
    scaled = mant * (1 << F)
    int_mant = math.floor(scaled); error = scaled - int_mant
    if biased == 0 and error != 0: env.flags |= UFC
    rm = env.rmode
    if rm == RN: up = error > Fr(1, 2) or (error == Fr(1, 2) and (int_mant & 1) == 1)
    elif rm == RP: up = error != 0 and sign == 0
    elif rm == RM: up = error != 0 and sign == 1
    else: up = False
    if up:
        int_mant += 1
        if int_mant == (1 << F): biased = 1
        if int_mant == (1 << (F + 1)):
            biased += 1; int_mant >>= 1
    if biased >= (1 << E) - 1:
        env.flags |= OFC | IXC
        to_inf = rm == RN or (rm == RP and sign == 0) or (rm == RM and sign == 1)
        return inf(f, sign) if to_inf else maxnorm(f, sign)
    if error != 0: env.flags |= IXC
    return (sign << (E + F)) | (biased << F) | (int_mant & ((1 << F) - 1))

def sval(c): return (-1 if c[1] else 1) * c[2]

def nan2(f, ca, cb, env):
    sel = None
    for c in (ca, cb):
        if c[0] == 'snan': sel = c; break
    if sel is None:
        for c in (ca, cb):
            if c[0] == 'qnan': sel = c; break
    if sel is None: return None
    if sel[0] == 'snan': env.flags |= IOC
    return defnan(f) if env.dn else quiet(f, sel[3])

def nan3(f, cc, ca, cb, env):
    sel = None
    for c in (cc, ca, cb):
        if c[0] == 'snan': sel = c; break
    if sel is None:
        for c in (cc, ca, cb):
            if c[0] == 'qnan': sel = c; break
    if sel is None: return None
    if sel[0] == 'snan': env.flags |= IOC
    return defnan(f) if env.dn else quiet(f, sel[3])

def addsub(f, a, b, env, sub=False):
    ca, cb = cls(f, a, env), cls(f, b, env)
    r = nan2(f, ca, cb, env)
    if r is not None: return r
    if sub: cb = (cb[0], cb[1] ^ 1, cb[2], cb[3])
    ia, ib = ca[0] == 'inf', cb[0] == 'inf'
    if ia and ib:
        if ca[1] == cb[1]: return inf(f, ca[1])
        env.flags |= IOC; return defnan(f)
    if ia: return inf(f, ca[1])
    if ib: return inf(f, cb[1])
    if ca[0] == 'zero' and cb[0] == 'zero':
        return zero(f, ca[1]) if ca[1] == cb[1] else zero(f, 1 if env.rmode == RM else 0)
    exact = sval(ca) + sval(cb)
    if exact == 0: return zero(f, 1 if env.rmode == RM else 0)
    return fp_round(f, 1 if exact < 0 else 0, abs(exact), env)

def mul(f, a, b, env):
    ca, cb = cls(f, a, env), cls(f, b, env)
    r = nan2(f, ca, cb, env)
    if r is not None: return r
    s = ca[1] ^ cb[1]
    ia, ib, za, zb = ca[0] == 'inf', cb[0] == 'inf', ca[0] == 'zero', cb[0] == 'zero'
    if (ia and zb) or (ib and za): env.flags |= IOC; return defnan(f)
    if ia or ib: return inf(f, s)
    if za or zb: return zero(f, s)
    return fp_round(f, s, ca[2] * cb[2], env)

def div(f, a, b, env):
    ca, cb = cls(f, a, env), cls(f, b, env)
    r = nan2(f, ca, cb, env)
    if r is not None: return r
    s = ca[1] ^ cb[1]
    ia, ib, za, zb = ca[0] == 'inf', cb[0] == 'inf', ca[0] == 'zero', cb[0] == 'zero'
    if (ia and ib) or (za and zb): env.flags |= IOC; return defnan(f)
    if ia: return inf(f, s)
    if zb: env.flags |= DZC; return inf(f, s)
    if ib or za: return zero(f, s)
    return fp_round(f, s, ca[2] / cb[2], env)

def fsqrt(f, a, env):
    c = cls(f, a, env)
    if c[0] in ('snan', 'qnan'):
        if c[0] == 'snan': env.flags |= IOC
        return defnan(f) if env.dn else quiet(f, c[3])
    if c[0] == 'zero': return zero(f, c[1])
    if c[1]: env.flags |= IOC; return defnan(f)
    if c[0] == 'inf': return inf(f, 0)
    x = c[2]; k = 1300
    num = x.numerator * (1 << (2 * k))
    N = num // x.denominator
    r = math.isqrt(N)
    exact = (num % x.denominator == 0) and (r * r == N)
    val = Fr(r, 1 << k) if exact else Fr(2 * r + 1, 1 << (k + 1))
    return fp_round(f, 0, val, env)

def fma(f, a, b, c, env):  # a*b + c
    ca, cb, cc = cls(f, a, env), cls(f, b, env), cls(f, c, env)
    ia, ib, ic = ca[0] == 'inf', cb[0] == 'inf', cc[0] == 'inf'
    za, zb, zc = ca[0] == 'zero', cb[0] == 'zero', cc[0] == 'zero'
    if cc[0] == 'qnan' and ((ia and zb) or (za and ib)): env.flags |= IOC; return defnan(f)
    r = nan3(f, cc, ca, cb, env)
    if r is not None: return r
    sp = ca[1] ^ cb[1]; infp = ia or ib; zerop = za or zb
    if (ia and zb) or (za and ib) or (ic and infp and cc[1] != sp): env.flags |= IOC; return defnan(f)
    if (ic and cc[1] == 0) or (infp and sp == 0): return inf(f, 0)
    if (ic and cc[1] == 1) or (infp and sp == 1): return inf(f, 1)
    if zc and zerop and cc[1] == sp: return zero(f, cc[1])
    exact = sval(cc) + (sval(ca) * sval(cb))
    if exact == 0: return zero(f, 1 if env.rmode == RM else 0)
    return fp_round(f, 1 if exact < 0 else 0, abs(exact), env)

def minmax(f, a, b, env, is_min, num):
    ca, cb = cls(f, a, env), cls(f, b, env)
    if num:
        if ca[0] == 'qnan' and cb[0] != 'qnan': ca = ('inf', 0 if is_min else 1, None, 0)
        elif cb[0] == 'qnan' and ca[0] != 'qnan': cb = ('inf', 0 if is_min else 1, None, 0)
    r = nan2(f, ca, cb, env)
    if r is not None: return r
    if ca[0] == 'zero' and cb[0] == 'zero':
        return zero(f, (ca[1] | cb[1]) if is_min else (ca[1] & cb[1]))
    def key(c):
        if c[0] == 'inf': return (math.inf if c[1] == 0 else -math.inf)
        return sval(c)
    ka, kb = key(ca), key(cb)
    pick = (ca if ka <= kb else cb) if is_min else (ca if ka >= kb else cb)
    if pick[0] == 'inf': return inf(f, pick[1])
    if pick[0] == 'zero': return zero(f, pick[1])
    return pick[3]

def compare(f, a, b, env, signaling):
    ca, cb = cls(f, a, env), cls(f, b, env)
    if ca[0] in ('snan', 'qnan') or cb[0] in ('snan', 'qnan'):
        if signaling or ca[0] == 'snan' or cb[0] == 'snan': env.flags |= IOC
        return 3
    def key(c):
        if c[0] == 'inf': return (math.inf if c[1] == 0 else -math.inf)
        return sval(c)
    ka, kb = key(ca), key(cb)
    return 6 if ka == kb else (8 if ka < kb else 2)

def convert(src, dst, a, env):
    c = cls(src, a, env)
    Es, Fs, _, _ = P(src); Ed, Fd, _, _ = P(dst)
    if c[0] in ('snan', 'qnan'):
        if c[0] == 'snan': env.flags |= IOC
        if env.dn: return defnan(dst)
        fs = a & ((1 << Fs) - 1)
        fd = (fs << (Fd - Fs)) if Fd >= Fs else (fs >> (Fs - Fd))
        fd |= 1 << (Fd - 1)
        return (c[1] << (Ed + Fd)) | (((1 << Ed) - 1) << Fd) | fd
    if c[0] == 'inf': return inf(dst, c[1])
    if c[0] == 'zero': return zero(dst, c[1])
    return fp_round(dst, c[1], c[2], env)

def roundint(f, a, rm, exact, env):
    c = cls(f, a, env)
    if c[0] in ('snan', 'qnan'):
        if c[0] == 'snan': env.flags |= IOC
        return defnan(f) if env.dn else quiet(f, c[3])
    if c[0] == 'inf': return inf(f, c[1])
    if c[0] == 'zero': return zero(f, c[1])
    value = sval(c); ir = math.floor(value); err = value - ir
    if rm == RN: up = err > Fr(1, 2) or (err == Fr(1, 2) and ir % 2 == 1)
    elif rm == RP: up = err != 0
    elif rm == RM: up = False
    elif rm == RZ: up = err != 0 and ir < 0
    else: up = err > Fr(1, 2) or (err == Fr(1, 2) and ir >= 0)
    if up: ir += 1
    if exact and err != 0: env.flags |= IXC
    if ir == 0: return zero(f, c[1])
    return fp_round(f, 1 if ir < 0 else 0, Fr(abs(ir)), Env())

def toint(f, a, signed, width, fbits, rm, env):
    c = cls(f, a, env)
    if c[0] in ('snan', 'qnan'):
        env.flags |= IOC; return 0
    if c[0] == 'inf': value = (-1 if c[1] else 1) * Fr(2) ** 1000000
    else: value = sval(c)
    value = value * (1 << fbits)
    ir = math.floor(value); err = value - ir
    if rm == RN: up = err > Fr(1, 2) or (err == Fr(1, 2) and ir % 2 == 1)
    elif rm == RP: up = err != 0
    elif rm == RM: up = False
    elif rm == RZ: up = err != 0 and ir < 0
    else: up = err > Fr(1, 2) or (err == Fr(1, 2) and ir >= 0)
    if up: ir += 1
    lo, hi = ((-(1 << (width - 1)), (1 << (width - 1)) - 1) if signed else (0, (1 << width) - 1))
    ovf = ir < lo or ir > hi
    res = max(lo, min(hi, ir))
    if ovf: env.flags |= IOC
    elif err != 0: env.flags |= IXC
    return res & ((1 << width) - 1)

def fromint(f, v, signed, width, fbits, env):
    if signed and v >> (width - 1): v -= 1 << width
    if v == 0: return 0
    value = Fr(v, 1 << fbits)
    return fp_round(f, 1 if value < 0 else 0, abs(value), env)

# ---------------- oracle'ın kendi doğrulaması: CPython yerel float'ları (RN) ----------------
def _unpack(f, bits):
    return struct.unpack({'s': '<f', 'd': '<d', 'h': '<e'}[f], bits.to_bytes({'s': 4, 'd': 8, 'h': 2}[f], 'little'))[0]
def _pack(f, x):
    try: return int.from_bytes(struct.pack({'s': '<f', 'd': '<d', 'h': '<e'}[f], x), 'little')
    except OverflowError:
        E, F, _, _ = P(f); return inf(f, 1 if x < 0 else 0)

def selfcheck(n=4000):
    rng = random.Random(7)
    def rnd(f):
        E, F, bias, _ = P(f)
        while True:
            s = rng.getrandbits(1)
            if rng.random() < 0.5: ef = max(1, min((1 << E) - 2, bias + rng.randint(-12, 12)))
            else: ef = rng.randint(0, (1 << E) - 2)
            return (s << (E + F)) | (ef << F) | rng.getrandbits(F)
    bad = 0
    for f in ('s', 'd'):
        for _ in range(n):
            a, b = rnd(f), rnd(f)
            x, y = _unpack(f, a), _unpack(f, b)
            for name, fn, nat in (("add", lambda e: addsub(f, a, b, e), lambda: x + y), ("sub", lambda e: addsub(f, a, b, e, True), lambda: x - y),
                                  ("mul", lambda e: mul(f, a, b, e), lambda: x * y)):
                got = fn(Env()); want = _pack(f, nat())
                if got != want: bad += 1; print("selfcheck FAIL", f, name, hex(a), hex(b), hex(got), hex(want))
            if y != 0:
                got = div(f, a, b, Env()); want = _pack(f, x / y)
                if got != want: bad += 1; print("selfcheck FAIL", f, "div", hex(a), hex(b), hex(got), hex(want))
            if x >= 0:
                got = fsqrt(f, a & ~(1 << (P(f)[0] + P(f)[1])), Env()); want = _pack(f, math.sqrt(abs(x)))
                if got != want: bad += 1; print("selfcheck FAIL", f, "sqrt", hex(a), hex(got), hex(want))
    for _ in range(n):  # d -> s -> h dönüşümleri
        a = rnd('d'); x = _unpack('d', a)
        for dst in ('s', 'h'):
            got = convert('d', dst, a, Env()); want = _pack(dst, x)
            if got != want: bad += 1; print("selfcheck FAIL cvt d->", dst, hex(a), hex(got), hex(want))
    if bad: raise SystemExit(f"oracle selfcheck başarısız: {bad}")
    print("oracle selfcheck OK (CPython yerel IEEE float ile eşleşti)")

if __name__ == "__main__":
    selfcheck()

# ------------- 2c-3 ek oracle işlevleri -------------
def two_c(f): return 0x40000000 if f == 's' else 0x4000000000000000
def mulx(f, a, b, env):
    e2 = Env(fz=env.fz); ca, cb = cls(f, a, e2), cls(f, b, e2)
    if (ca[0] == 'inf' and cb[0] == 'zero') or (ca[0] == 'zero' and cb[0] == 'inf'):
        env.flags |= e2.flags; E, F, _, _ = P(f)
        return two_c(f) | ((ca[1] ^ cb[1]) << (E + F))
    return mul(f, a, b, env)
def recps(f, a, b, env):
    E, F, _, _ = P(f); a ^= 1 << (E + F)
    ca, cb = cls(f, a, env), cls(f, b, env)
    r = nan2(f, ca, cb, env)
    if r is not None: return r
    ia, ib, za, zb = ca[0] == 'inf', cb[0] == 'inf', ca[0] == 'zero', cb[0] == 'zero'
    if (ia and zb) or (za and ib): return two_c(f)
    if ia or ib: return inf(f, ca[1] ^ cb[1])
    exact = 2 + sval(ca) * sval(cb)
    return zero(f, 0) if exact == 0 else fp_round(f, 1 if exact < 0 else 0, abs(exact), env)
def rsqrts(f, a, b, env):
    E, F, _, _ = P(f); a ^= 1 << (E + F)
    ca, cb = cls(f, a, env), cls(f, b, env)
    r = nan2(f, ca, cb, env)
    if r is not None: return r
    ia, ib, za, zb = ca[0] == 'inf', cb[0] == 'inf', ca[0] == 'zero', cb[0] == 'zero'
    onep5 = 0x3FC00000 if f == 's' else 0x3FF8000000000000
    if (ia and zb) or (za and ib): return onep5
    if ia or ib: return inf(f, ca[1] ^ cb[1])
    exact = (3 + sval(ca) * sval(cb)) / 2
    return zero(f, 0) if exact == 0 else fp_round(f, 1 if exact < 0 else 0, abs(exact), env)

def _recip_int(a):
    a = a * 2 + 1; b = (1 << 19) // a; return (b + 1) // 2
def _rsqrt_int(a):
    if a < 256: a = a * 2 + 1
    else: a = (a >> 1) << 1; a = (a + 1) * 2
    b = 512
    while a * (b + 1) * (b + 1) < (1 << 28): b += 1
    return (b + 1) // 2
def recpe(f, a, env):
    E, F, bias, _ = P(f); c = cls(f, a, env)
    if c[0] in ('snan', 'qnan'):
        if c[0] == 'snan': env.flags |= IOC
        return defnan(f) if env.dn else quiet(f, c[3])
    if c[0] == 'inf': return zero(f, c[1])
    if c[0] == 'zero': env.flags |= DZC; return inf(f, c[1])
    tiny = -128 if f == 's' else -1024
    if c[2] < Fr(2) ** tiny:
        env.flags |= OFC | IXC
        to_inf = env.rmode == RN or (env.rmode == RP and c[1] == 0) or (env.rmode == RM and c[1] == 1)
        return inf(f, c[1]) if to_inf else maxnorm(f, c[1])
    if env.fz and ((f == 's' and c[2] >= Fr(2) ** 126) or (f == 'd' and c[2] >= Fr(2) ** 1022)):
        env.flags |= UFC; return zero(f, c[1])
    frac = (a & ((1 << F) - 1)) << (52 - F); ex = (a >> F) & ((1 << E) - 1)
    if ex == 0:
        if (frac >> 51) & 1 == 0: ex = -1; frac = (frac & ((1 << 50) - 1)) << 2
        else: frac = (frac & ((1 << 51) - 1)) << 1
    scaled = 256 | ((frac >> 44) & 0xFF)
    rexp = (253 if f == 's' else 2045) - ex
    est = _recip_int(scaled); fr_ = (est & 0xFF) << 44
    if rexp == 0: fr_ = (1 << 51) | (fr_ >> 1)
    elif rexp == -1: fr_ = (1 << 50) | (fr_ >> 2); rexp = 0
    return (c[1] << (E + F)) | (rexp << F) | (fr_ >> (52 - F))
def rsqrte(f, a, env):
    E, F, bias, _ = P(f); c = cls(f, a, env)
    if c[0] in ('snan', 'qnan'):
        if c[0] == 'snan': env.flags |= IOC
        return defnan(f) if env.dn else quiet(f, c[3])
    if c[0] == 'zero': env.flags |= DZC; return inf(f, c[1])
    if c[1]: env.flags |= IOC; return defnan(f)
    if c[0] == 'inf': return zero(f, 0)
    frac = (a & ((1 << F) - 1)) << (52 - F); ex = (a >> F) & ((1 << E) - 1)
    if ex == 0:
        while (frac >> 51) & 1 == 0: frac = (frac & ((1 << 51) - 1)) << 1; ex -= 1
        frac = (frac & ((1 << 51) - 1)) << 1
    scaled = (256 | ((frac >> 44) & 0xFF)) if ex % 2 == 0 else (128 | ((frac >> 45) & 0x7F))
    rexp = ((380 if f == 's' else 3068) - ex) // 2
    est = _rsqrt_int(scaled)
    return (rexp << F) | (((est & 0xFF) << 44) >> (52 - F))
