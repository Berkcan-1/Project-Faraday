"""ASIMD 2c-2b/2c-3 test vektörleri (HAFİF: şablon başına az vaka). Kodlama: assembler; beklenen sonuç: lane tabanlı Python oracle (+ fpo.py)."""
import os, random, tempfile
import fpo
from fpo import Env, RN, RP, RM, RZ, RA
from gen_fpvectors import SPECIAL, FPCRS
from gen_asimd import ARR, geo, lanes, pack, mask, sx, M64, M128, dmask

POISON = 0xDEADBEEFCAFEF00D_A5A5A5A5A5A5A5A5
AFMT = {'2s': ('s', 32, 2), '4s': ('s', 32, 4), '2d': ('d', 64, 2)}
SIGN = {'s': 1 << 31, 'd': 1 << 63}

class G:
    def __init__(self): self.progs, self.pidx, self.cases, self.rng, self.k = [], {}, [], random.Random(777), 0
    def prog(self, a):
        if a not in self.pidx: self.pidx[a] = len(self.progs); self.progs.append(a)
        return self.pidx[a]
    def rv(self):
        b = [(0x00 if c < .1 else 0xFF if c < .2 else 0x80 if c < .3 else 0x7F if c < .4 else self.rng.getrandbits(8)) for c in (self.rng.random() for _ in range(16))]
        return sum(x << (8 * i) for i, x in enumerate(b))
    def rvf(self, f):
        e = 32 if f == 's' else 64; n = 128 // e; ls = []
        for _ in range(n):
            c = self.rng.random()
            if c < .45: ls.append(self.rng.choice(SPECIAL[f]))
            elif c < .75:
                E, F, bias, _ = fpo.P(f)
                ls.append((self.rng.getrandbits(1) << (E + F)) | (max(1, min((1 << E) - 2, bias + self.rng.randint(-8, 8))) << F) | self.rng.getrandbits(F))
            else: ls.append(self.rng.getrandbits(e))
        return pack(ls, e)
    def emit(self, asm, fn, fmt=None, k=3, fp=False):
        pi = self.prog(asm)
        for _ in range(k):
            self.k += 1
            fpcr = FPCRS[self.k % len(FPCRS)] if (fmt or fp) else 0
            v = [self.rvf(fmt) if fmt else self.rv() for _ in range(6)]
            d = self.rvf(fmt) if fmt else self.rng.choice([self.rv(), POISON])
            env = Env.from_fpcr(fpcr); q = [False]
            out = fn(d, v, env, q) & M128
            self.cases.append((pi, fpcr, env.flags | ((1 << 27) if q[0] else 0), v, d, out))

def satS(v, e, q):
    lo, hi = -(1 << (e - 1)), (1 << (e - 1)) - 1
    if v < lo or v > hi: q[0] = True
    return max(lo, min(hi, v)) & mask(e)
def satU(v, e, q):
    hi = (1 << e) - 1
    if v < 0 or v > hi: q[0] = True
    return max(0, min(hi, v))

def lw(arr, f, scalar=None):
    q_, e, n = geo(arr)
    def fn(d, v, env, q):
        nn = 1 if scalar else n
        r = [f(a, b, dd, e, env, q) for a, b, dd in zip(lanes(v[0], e, nn), lanes(v[1], e, nn), lanes(d, e, nn))]
        return pack(r, e) if scalar else pack(r, e) & dmask(arr)
    return fn

def build():
    g = G()
    # ================= FP üç-aynı =================
    def fpbin(op, fm):
        sign = SIGN[fm]; e = 32 if fm == 's' else 64
        def f(a, b, d, e_, env, q):
            if op == 'add': return fpo.addsub(fm, a, b, env)
            if op == 'sub': return fpo.addsub(fm, a, b, env, True)
            if op == 'mul': return fpo.mul(fm, a, b, env)
            if op == 'div': return fpo.div(fm, a, b, env)
            if op == 'max': return fpo.minmax(fm, a, b, env, False, False)
            if op == 'min': return fpo.minmax(fm, a, b, env, True, False)
            if op == 'maxnm': return fpo.minmax(fm, a, b, env, False, True)
            if op == 'minnm': return fpo.minmax(fm, a, b, env, True, True)
            if op == 'abd': return fpo.addsub(fm, a, b, env, True) & ~sign
            if op == 'mulx': return fpo.mulx(fm, a, b, env)
            if op == 'recps': return fpo.recps(fm, a, b, env)
            if op == 'rsqrts': return fpo.rsqrts(fm, a, b, env)
            if op == 'fmla': return fpo.fma(fm, b, a, d, env) if False else fpo.fma(fm, a, b, d, env)
            if op == 'fmls': return fpo.fma(fm, a ^ sign, b, d, env)
            m = mask(e)
            if op == 'eq': return m if fpo.compare(fm, a, b, env, False) == 6 else 0
            z = fpo.compare(fm, a & ~sign if op in ('age', 'agt') else a, b & ~sign if op in ('age', 'agt') else b, env, True)
            return m if (z in (2, 6) if op in ('ge', 'age') else z == 2) else 0
        return f
    for arr, (fm, e, n) in AFMT.items():
        for op, name in (('add', 'fadd'), ('sub', 'fsub'), ('mul', 'fmul'), ('div', 'fdiv'), ('max', 'fmax'), ('min', 'fmin'), ('maxnm', 'fmaxnm'), ('minnm', 'fminnm'),
                         ('abd', 'fabd'), ('mulx', 'fmulx'), ('recps', 'frecps'), ('rsqrts', 'frsqrts'), ('fmla', 'fmla'), ('fmls', 'fmls'),
                         ('eq', 'fcmeq'), ('ge', 'fcmge'), ('gt', 'fcmgt'), ('age', 'facge'), ('agt', 'facgt')):
            g.emit(f"{name} v0.{arr}, v1.{arr}, v2.{arr}", lw(arr, fpbin(op, fm)), fmt=fm)
        for op, name in (('add', 'faddp'), ('max', 'fmaxp'), ('min', 'fminp'), ('maxnm', 'fmaxnmp'), ('minnm', 'fminnmp')):
            def pw(d, v, env, q, op=op, fm=fm, e=e, n=n):
                a, b = lanes(v[0], e, n), lanes(v[1], e, n); f = fpbin(op, fm)
                return pack([f(src[2 * i], src[2 * i + 1], 0, e, env, q) for src in (a, b) for i in range(n // 2)], e) & dmask(arr)
            g.emit(f"{name} v0.{arr}, v1.{arr}, v2.{arr}", pw, fmt=fm)
    # skaler FP üç-aynı (SIMD skaler sınıfı)
    for fm, rg in (('s', 's'), ('d', 'd')):
        e = 32 if fm == 's' else 64
        for op, name in (('abd', 'fabd'), ('mulx', 'fmulx'), ('recps', 'frecps'), ('rsqrts', 'frsqrts'), ('eq', 'fcmeq'), ('ge', 'fcmge'), ('gt', 'fcmgt'), ('age', 'facge'), ('agt', 'facgt')):
            g.emit(f"{name} {rg}0, {rg}1, {rg}2", lw({'s': '2s', 'd': '2d'}[fm], fpbin(op, fm), scalar=True), fmt=fm, k=2)
    for arr, (fm, e, n) in AFMT.items():
        f_ = fpbin
        # ================= FP iki-register =================
        U1 = {
            'fabs': lambda a, env, q: a & ~SIGN[fm], 'fneg': lambda a, env, q: a ^ SIGN[fm], 'fsqrt': lambda a, env, q: fpo.fsqrt(fm, a, env),
            'frintn': lambda a, env, q: fpo.roundint(fm, a, RN, False, env), 'frintp': lambda a, env, q: fpo.roundint(fm, a, RP, False, env),
            'frintm': lambda a, env, q: fpo.roundint(fm, a, RM, False, env), 'frintz': lambda a, env, q: fpo.roundint(fm, a, RZ, False, env),
            'frinta': lambda a, env, q: fpo.roundint(fm, a, RA, False, env), 'frintx': lambda a, env, q: fpo.roundint(fm, a, env.rmode, True, env),
            'frinti': lambda a, env, q: fpo.roundint(fm, a, env.rmode, False, env),
            'frecpe': lambda a, env, q: fpo.recpe(fm, a, env), 'frsqrte': lambda a, env, q: fpo.rsqrte(fm, a, env),
        }
        for nm, rm_ in (('n', RN), ('m', RM), ('p', RP), ('z', RZ), ('a', RA)):
            for sg, sgn in (('s', True), ('u', False)):
                U1[f'fcvt{nm}{sg}'] = lambda a, env, q, rm_=rm_, sgn=sgn: fpo.toint(fm, a, sgn, e, 0, rm_, env)
        U1['scvtf'] = lambda a, env, q: fpo.fromint(fm, a, True, e, 0, env)
        U1['ucvtf'] = lambda a, env, q: fpo.fromint(fm, a, False, e, 0, env)
        for nm, f in U1.items():
            g.emit(f"{nm} v0.{arr}, v1.{arr}", lambda d, v, env, q, f=f, e=e, n=n, arr=arr: pack([f(a, env, q) for a in lanes(v[0], e, n)], e) & dmask(arr), fmt=fm, k=2)
        for nm, t in (('fcmgt', lambda z: z == 2), ('fcmge', lambda z: z in (2, 6)), ('fcmeq', lambda z: z == 6), ('fcmle', lambda z: z in (6, 8)), ('fcmlt', lambda z: z == 8)):
            g.emit(f"{nm} v0.{arr}, v1.{arr}, #0.0",
                   lambda d, v, env, q, t=t, nm=nm, e=e, n=n, arr=arr: pack([mask(e) if t(fpo.compare(fm, a, 0, env, nm != 'fcmeq')) else 0 for a in lanes(v[0], e, n)], e) & dmask(arr), fmt=fm, k=2)
        for fb in (3, e // 2):
            for nm, sgn in (('scvtf', True), ('ucvtf', False)):
                g.emit(f"{nm} v0.{arr}, v1.{arr}, #{fb}", lambda d, v, env, q, sgn=sgn, fb=fb, e=e, n=n, arr=arr: pack([fpo.fromint(fm, a, sgn, e, fb, env) for a in lanes(v[0], e, n)], e) & dmask(arr), fmt=fm, k=2)
            for nm, sgn in (('fcvtzs', True), ('fcvtzu', False)):
                g.emit(f"{nm} v0.{arr}, v1.{arr}, #{fb}", lambda d, v, env, q, sgn=sgn, fb=fb, e=e, n=n, arr=arr: pack([fpo.toint(fm, a, sgn, e, fb, RZ, env) for a in lanes(v[0], e, n)], e) & dmask(arr), fmt=fm, k=2)
    # skaler iki-register FP
    for fm, rg, arr in (('s', 's', '4s'), ('d', 'd', '2d')):
        e = 32 if fm == 's' else 64
        for nm, f in (('fcvtzs', lambda a, env, q: fpo.toint(fm, a, True, e, 0, RZ, env)), ('fcvtzu', lambda a, env, q: fpo.toint(fm, a, False, e, 0, RZ, env)),
                      ('fcvtas', lambda a, env, q: fpo.toint(fm, a, True, e, 0, RA, env)), ('scvtf', lambda a, env, q: fpo.fromint(fm, a, True, e, 0, env)),
                      ('ucvtf', lambda a, env, q: fpo.fromint(fm, a, False, e, 0, env)), ('frecpe', lambda a, env, q: fpo.recpe(fm, a, env)),
                      ('frsqrte', lambda a, env, q: fpo.rsqrte(fm, a, env))):
            g.emit(f"{nm} {rg}0, {rg}1", lambda d, v, env, q, f=f, e=e: f(v[0] & mask(e), env, q), fmt=fm, k=2)
        g.emit(f"fcmeq {rg}0, {rg}1, #0.0", lambda d, v, env, q, fm=fm, e=e: mask(e) if fpo.compare(fm, v[0] & mask(e), 0, env, False) == 6 else 0, fmt=fm, k=2)
        g.emit(f"scvtf {rg}0, {rg}1, #5", lambda d, v, env, q, fm=fm, e=e: fpo.fromint(fm, v[0] & mask(e), True, e, 5, env), fmt=fm, k=2)
        g.emit(f"fcvtzu {rg}0, {rg}1, #9", lambda d, v, env, q, fm=fm, e=e: fpo.toint(fm, v[0] & mask(e), False, e, 9, RZ, env), fmt=fm, k=2)
    # fcvtn/fcvtl/fcvtxn, across-lanes, skaler pairwise, vektör FMOV imm, by-element FP
    for dst, src, hi, sm, bg in (('4h', '4s', 0, 'h', 's'), ('8h', '4s', 1, 'h', 's'), ('2s', '2d', 0, 's', 'd'), ('4s', '2d', 1, 's', 'd')):
        be = 32 if bg == 's' else 64; se = be // 2; cnt = 64 // se
        g.emit(f"fcvtn{'2' if hi else ''} v0.{dst}, v1.{src}", lambda d, v, env, q, hi=hi, be=be, se=se, cnt=cnt, sm=sm, bg=bg:
               ((d & M64) if hi else 0) | (pack([fpo.convert(bg, sm, a, env) for a in lanes(v[0], be, cnt)], se) << (64 if hi else 0)), fmt=bg, k=3)
        g.emit(f"fcvtl{'2' if hi else ''} v0.{src}, v1.{dst}", lambda d, v, env, q, hi=hi, be=be, se=se, cnt=cnt, sm=sm, bg=bg:
               pack([fpo.convert(sm, bg, a, env) for a in lanes(v[0] >> (64 if hi else 0), se, cnt)], be), fp=True, k=3)
    for nm, op in (('fmaxv', 'max'), ('fminv', 'min'), ('fmaxnmv', 'maxnm'), ('fminnmv', 'minnm')):
        def red(d, v, env, q, op=op):
            l = lanes(v[0], 32, 4); f = fpbin(op, 's')
            return f(f(l[0], l[1], 0, 32, env, q), f(l[2], l[3], 0, 32, env, q), 0, 32, env, q)
        g.emit(f"{nm} s0, v1.4s", red, fmt='s', k=3)
    for nm, op in (('faddp', 'add'), ('fmaxp', 'max'), ('fminp', 'min'), ('fmaxnmp', 'maxnm'), ('fminnmp', 'minnm')):
        for rg, fm, e, arr in (('s', 's', 32, '2s'), ('d', 'd', 64, '2d')):
            g.emit(f"{nm} {rg}0, v1.{arr}", lambda d, v, env, q, op=op, fm=fm, e=e: fpbin(op, fm)(lanes(v[0], e, 2)[0], lanes(v[0], e, 2)[1], 0, e, env, q), fmt=fm, k=2)
    for x in (1.0, -2.5, 0.125, 31.0, -0.1875, 10.0):
        import struct
        for arr, fm, e, n, f_ in (('4s', 's', 32, 4, '<f'), ('2s', 's', 32, 2, '<f'), ('2d', 'd', 64, 2, '<d')):
            bits = int.from_bytes(struct.pack(f_, x), 'little')
            g.emit(f"fmov v0.{arr}, #{x}", lambda d, v, env, q, bits=bits, e=e, n=n, arr=arr: pack([bits] * n, e) & dmask(arr), k=1)
    for arr, (fm, e, n) in AFMT.items():
        for idx in range(min(n, 2)):
            ch = 's' if fm == 's' else 'd'
            for nm, op in (('fmul', 'mul'), ('fmla', 'fmla'), ('fmls', 'fmls'), ('fmulx', 'mulx')):
                def be(d, v, env, q, op=op, fm=fm, e=e, n=n, idx=idx, arr=arr):
                    el = lanes(v[1], e, 128 // e)[idx]; f = fpbin(op, fm)
                    return pack([f(a, el, dd, e, env, q) for a, dd in zip(lanes(v[0], e, n), lanes(d, e, n))], e) & dmask(arr)
                g.emit(f"{nm} v0.{arr}, v1.{arr}, v2.{ch}[{idx}]", be, fmt=fm, k=2)
        for nm, op in (('fmul', 'mul'), ('fmla', 'fmla')):
            rg = 's' if fm == 's' else 'd'
            g.emit(f"{nm} {rg}0, {rg}1, v2.{rg}[1]", lambda d, v, env, q, op=op, fm=fm, e=e: fpbin(op, fm)(v[0] & mask(e), lanes(v[1], e, 128 // e)[1], d & mask(e), e, env, q), fmt=fm, k=2)
    # ================= doyurucu =================
    def sat_ops():
        def mk3(name, f): return (name, f)
        def shl(a, b, e, signed, rnd, q):
            sh = sx(b, 8); val = sx(a, e) if signed else a
            if sh >= 0:
                r = val << sh if sh < 130 else (val << 130)
            else:
                rs = -sh; r = (val + ((1 << (rs - 1)) if rnd else 0)) >> rs
            return satS(r, e, q) if signed else satU(r, e, q)
        return [
            ('sqadd', lambda a, b, d, e, env, q: satS(sx(a, e) + sx(b, e), e, q)), ('uqadd', lambda a, b, d, e, env, q: satU(a + b, e, q)),
            ('sqsub', lambda a, b, d, e, env, q: satS(sx(a, e) - sx(b, e), e, q)), ('uqsub', lambda a, b, d, e, env, q: satU(a - b, e, q)),
            ('sqshl', lambda a, b, d, e, env, q: shl(a, b, e, True, False, q)), ('uqshl', lambda a, b, d, e, env, q: shl(a, b, e, False, False, q)),
            ('sqrshl', lambda a, b, d, e, env, q: shl(a, b, e, True, True, q)), ('uqrshl', lambda a, b, d, e, env, q: shl(a, b, e, False, True, q)),
        ]
    for name, f in sat_ops():
        for arr in ('8b', '16b', '4h', '8h', '2s', '4s', '2d'):
            g.emit(f"{name} v0.{arr}, v1.{arr}, v2.{arr}", lw(arr, f), k=3)
        for rg, arr in (('b', '16b'), ('h', '8h'), ('s', '4s'), ('d', '2d')):
            g.emit(f"{name} {rg}0, {rg}1, {rg}2", lw(arr, f, scalar=True), k=3)
    for nm, rnd in (('sqdmulh', 0), ('sqrdmulh', 1)):
        for arr in ('4h', '8h', '2s', '4s'):
            g.emit(f"{nm} v0.{arr}, v1.{arr}, v2.{arr}", lw(arr, lambda a, b, d, e, env, q, rnd=rnd: satS((2 * sx(a, e) * sx(b, e) + (rnd << (e - 1))) >> e, e, q)), k=3)
        for rg, arr in (('h', '8h'), ('s', '4s')):
            g.emit(f"{nm} {rg}0, {rg}1, {rg}2", lw(arr, lambda a, b, d, e, env, q, rnd=rnd: satS((2 * sx(a, e) * sx(b, e) + (rnd << (e - 1))) >> e, e, q), scalar=True), k=3)
    for arr in ('8b', '16b', '4h', '8h', '2s', '4s', '2d'):
        q_, e, n = geo(arr)
        g.emit(f"sqabs v0.{arr}, v1.{arr}", lambda d, v, env, q, e=e, n=n, arr=arr: pack([satS(abs(sx(a, e)), e, q) for a in lanes(v[0], e, n)], e) & dmask(arr), k=3)
        g.emit(f"sqneg v0.{arr}, v1.{arr}", lambda d, v, env, q, e=e, n=n, arr=arr: pack([satS(-sx(a, e), e, q) for a in lanes(v[0], e, n)], e) & dmask(arr), k=3)
        g.emit(f"suqadd v0.{arr}, v1.{arr}", lambda d, v, env, q, e=e, n=n, arr=arr: pack([satS(sx(dd, e) + a, e, q) for a, dd in zip(lanes(v[0], e, n), lanes(d, e, n))], e) & dmask(arr), k=3)
        g.emit(f"usqadd v0.{arr}, v1.{arr}", lambda d, v, env, q, e=e, n=n, arr=arr: pack([satU(dd + sx(a, e), e, q) for a, dd in zip(lanes(v[0], e, n), lanes(d, e, n))], e) & dmask(arr), k=3)
    for dst, src, hi in (('8b', '8h', 0), ('4h', '4s', 0), ('2s', '2d', 0), ('16b', '8h', 1), ('8h', '4s', 1), ('4s', '2d', 1)):
        qd, ed, nd = geo(dst); half = 64 // ed
        for nm, sf_ in (('sqxtn', lambda a, e, q: satS(sx(a, 2 * e), e, q)), ('uqxtn', lambda a, e, q: satU(a, e, q)), ('sqxtun', lambda a, e, q: satU(sx(a, 2 * e), e, q))):
            def fn(d, v, env, q, hi=hi, ed=ed, half=half, sf_=sf_):
                nar = [sf_(a, ed, q) for a in lanes(v[0], ed * 2, half)]
                return ((d & M64) if hi else 0) | (pack(nar, ed) << (64 if hi else 0))
            g.emit(f"{nm}{'2' if hi else ''} v0.{dst}, v1.{src}", fn, k=3)
    for rg, sg, sr in (('b', 'h', 8), ('h', 's', 16), ('s', 'd', 32)):
        g.emit(f"sqxtn {rg}0, {sg}1", lambda d, v, env, q, sr=sr: satS(sx(v[0] & mask(2 * sr), 2 * sr), sr, q), k=3)
    for arr in ('8b', '16b', '4h', '8h', '2s', '4s', '2d'):
        q_, e, n = geo(arr)
        for sh in sorted({1, e // 2, e - 1}):
            g.emit(f"sqshl v0.{arr}, v1.{arr}, #{sh}", lambda d, v, env, q, e=e, n=n, sh=sh, arr=arr: pack([satS(sx(a, e) << sh, e, q) for a in lanes(v[0], e, n)], e) & dmask(arr), k=2)
            g.emit(f"uqshl v0.{arr}, v1.{arr}, #{sh}", lambda d, v, env, q, e=e, n=n, sh=sh, arr=arr: pack([satU(a << sh, e, q) for a in lanes(v[0], e, n)], e) & dmask(arr), k=2)
            g.emit(f"sqshlu v0.{arr}, v1.{arr}, #{sh}", lambda d, v, env, q, e=e, n=n, sh=sh, arr=arr: pack([satU(sx(a, e) << sh, e, q) for a in lanes(v[0], e, n)], e) & dmask(arr), k=2)
    for dst, src, hi in (('8b', '8h', 0), ('4h', '4s', 0), ('2s', '2d', 0), ('16b', '8h', 1), ('8h', '4s', 1), ('4s', '2d', 1)):
        qd, ed, nd = geo(dst); half = 64 // ed
        for sh in sorted({1, ed // 2, ed}):
            for nm, signed_src, unsigned_dst, rnd in (('sqshrn', True, False, 0), ('sqrshrn', True, False, 1), ('uqshrn', False, True, 0), ('uqrshrn', False, True, 1), ('sqshrun', True, True, 0), ('sqrshrun', True, True, 1)):
                def fn(d, v, env, q, hi=hi, ed=ed, half=half, sh=sh, ss=signed_src, ud=unsigned_dst, rnd=rnd):
                    nar = []
                    for a in lanes(v[0], ed * 2, half):
                        x = (sx(a, ed * 2) if ss else a) + ((1 << (sh - 1)) if rnd else 0)
                        x >>= sh
                        nar.append(satU(x, ed, q) if ud else satS(x, ed, q))
                    return ((d & M64) if hi else 0) | (pack(nar, ed) << (64 if hi else 0))
                g.emit(f"{nm}{'2' if hi else ''} v0.{dst}, v1.{src}, #{sh}", fn, k=2)
    # skaler tamsayı D-formları (vektör çekirdeği lane 0)
    for nm, f in (('add', lambda a, b: a + b), ('sub', lambda a, b: a - b), ('cmeq', lambda a, b: M64 if a == b else 0), ('cmgt', lambda a, b: M64 if sx(a, 64) > sx(b, 64) else 0),
                  ('cmhi', lambda a, b: M64 if a > b else 0), ('cmtst', lambda a, b: M64 if a & b else 0)):
        g.emit(f"{nm} d0, d1, d2", lambda d, v, env, q, f=f: f(v[0] & M64, v[1] & M64) & M64, k=3)
    for nm, f in (('abs', lambda a: abs(sx(a, 64))), ('neg', lambda a: -sx(a, 64))):
        g.emit(f"{nm} d0, d1", lambda d, v, env, q, f=f: f(v[0] & M64) & M64, k=3)
    for sh in (1, 17, 63):
        g.emit(f"shl d0, d1, #{sh}", lambda d, v, env, q, sh=sh: (v[0] << sh) & M64, k=2)
        g.emit(f"ushr d0, d1, #{sh}", lambda d, v, env, q, sh=sh: (v[0] & M64) >> sh, k=2)
        g.emit(f"sshr d0, d1, #{sh}", lambda d, v, env, q, sh=sh: (sx(v[0], 64) >> sh) & M64, k=2)
    # ================= uzun / geniş / dar =================
    LW = [('8h', '8b', '16b', 8), ('4s', '4h', '8h', 16), ('2d', '2s', '4s', 32)]
    def ext(a, e, s): return sx(a, e) if s else a
    for wide, nar, nar2, e in LW:
        we = 2 * e; wn = 128 // we
        for hi, nr in ((0, nar), (1, nar2)):
            sfx = '2' if hi else ''
            def src(v, which, e=e, wn=wn, hi=hi): return lanes(v[which] >> (64 if hi else 0), e, wn)
            for pre, s in (('s', True), ('u', False)):
                for nm, f in (('addl', lambda a, b, dd, e, we, s: ext(a, e, s) + ext(b, e, s)), ('subl', lambda a, b, dd, e, we, s: ext(a, e, s) - ext(b, e, s)),
                              ('abdl', lambda a, b, dd, e, we, s: abs(ext(a, e, s) - ext(b, e, s))), ('abal', lambda a, b, dd, e, we, s: dd + abs(ext(a, e, s) - ext(b, e, s))),
                              ('mlal', lambda a, b, dd, e, we, s: dd + ext(a, e, s) * ext(b, e, s)), ('mlsl', lambda a, b, dd, e, we, s: dd - ext(a, e, s) * ext(b, e, s)),
                              ('mull', lambda a, b, dd, e, we, s: ext(a, e, s) * ext(b, e, s))):
                    def fn(d, v, env, q, f=f, e=e, we=we, wn=wn, s=s, hi=hi):
                        a, b, dl = lanes(v[0] >> (64 if hi else 0), e, wn), lanes(v[1] >> (64 if hi else 0), e, wn), lanes(d, we, wn)
                        return pack([f(x, y, dd, e, we, s) for x, y, dd in zip(a, b, dl)], we)
                    g.emit(f"{pre}{nm}{sfx} v0.{wide}, v1.{nr}, v2.{nr}", fn, k=2)
                for nm, f in (('addw', lambda a, b, e, we, s: ext(a, we, s) + ext(b, e, s)), ('subw', lambda a, b, e, we, s: ext(a, we, s) - ext(b, e, s))):
                    def fn(d, v, env, q, f=f, e=e, we=we, wn=wn, s=s, hi=hi):
                        a, b = lanes(v[0], we, wn), lanes(v[1] >> (64 if hi else 0), e, wn)
                        return pack([f(x, y, e, we, s) for x, y in zip(a, b)], we)
                    g.emit(f"{pre}{nm}{sfx} v0.{wide}, v1.{wide}, v2.{nr}", fn, k=2)
            if e >= 16:
                for nm, kind in (('sqdmull', 0), ('sqdmlal', 1), ('sqdmlsl', 2)):
                    def fn(d, v, env, q, kind=kind, e=e, we=we, wn=wn, hi=hi):
                        a, b, dl = lanes(v[0] >> (64 if hi else 0), e, wn), lanes(v[1] >> (64 if hi else 0), e, wn), lanes(d, we, wn); out = []
                        for x, y, dd in zip(a, b, dl):
                            p = sx(satS(2 * sx(x, e) * sx(y, e), we, q), we)
                            out.append(satS(p, we, q) if kind == 0 else satS(sx(dd, we) + (p if kind == 1 else -p), we, q))
                        return pack(out, we)
                    g.emit(f"{nm}{sfx} v0.{wide}, v1.{nr}, v2.{nr}", fn, k=2)
        # dar: addhn/raddhn/subhn/rsubhn
        for hi, nr in ((0, nar), (1, nar2)):
            for nm, sub, rnd in (('addhn', 0, 0), ('raddhn', 0, 1), ('subhn', 1, 0), ('rsubhn', 1, 1)):
                dst = nr if hi else nar
                def fn(d, v, env, q, sub=sub, rnd=rnd, e=e, we=we, wn=wn, hi=hi):
                    a, b = lanes(v[0], we, wn), lanes(v[1], we, wn); rc = (1 << (e - 1)) if rnd else 0
                    out = [(((x - y) if sub else (x + y)) + rc) & mask(we) for x, y in zip(a, b)]
                    return ((d & M64) if hi else 0) | (pack([o >> e for o in out], e) << (64 if hi else 0))
                g.emit(f"{nm}{'2' if hi else ''} v0.{dst}, v1.{wide}, v2.{wide}", fn, k=2)
    g.emit("pmull v0.8h, v1.8b, v2.8b", lambda d, v, env, q: pack([(lambda a, b: __import__('functools').reduce(lambda r, k: r ^ ((a << k) if (b >> k) & 1 else 0), range(8), 0))(x, y) for x, y in zip(lanes(v[0], 8, 8), lanes(v[1], 8, 8))], 16), k=3)
    # by-element (tamsayı)
    for arr, e, n in (('4h', 16, 4), ('8h', 16, 8), ('2s', 32, 2), ('4s', 32, 4)):
        for idx in (0, 3 if e == 16 else 1):
            rg = 'v2' if e == 32 else 'v2'
            ch = 'h' if e == 16 else 's'
            for nm, f in (('mul', lambda a, el, dd, e, q: a * el), ('mla', lambda a, el, dd, e, q: dd + a * el), ('mls', lambda a, el, dd, e, q: dd - a * el),
                          ('sqdmulh', lambda a, el, dd, e, q: satS((2 * sx(a, e) * sx(el, e)) >> e, e, q)), ('sqrdmulh', lambda a, el, dd, e, q: satS((2 * sx(a, e) * sx(el, e) + (1 << (e - 1))) >> e, e, q))):
                def fn(d, v, env, q, f=f, e=e, n=n, idx=idx, arr=arr):
                    el = lanes(v[1], e, 128 // e)[idx]
                    return pack([f(a, el, dd, e, q) for a, dd in zip(lanes(v[0], e, n), lanes(d, e, n))], e) & dmask(arr)
                g.emit(f"{nm} v0.{arr}, v1.{arr}, v2.{ch}[{idx}]", fn, k=2)
    for wide, nar, nar2, e in LW[1:]:
        we = 2 * e; wn = 128 // we; ch = 'h' if e == 16 else 's'
        for hi, nr in ((0, nar), (1, nar2)):
            sfx = '2' if hi else ''
            for pre, s in (('s', True), ('u', False)):
                for nm, f in (('mull', lambda a, el, dd: a * el), ('mlal', lambda a, el, dd: dd + a * el), ('mlsl', lambda a, el, dd: dd - a * el)):
                    def fn(d, v, env, q, f=f, e=e, we=we, wn=wn, s=s, hi=hi):
                        el = lanes(v[1], e, 128 // e)[1]
                        return pack([f(ext(a, e, s), ext(el, e, s), dd) for a, dd in zip(lanes(v[0] >> (64 if hi else 0), e, wn), lanes(d, we, wn))], we)
                    g.emit(f"{pre}{nm}{sfx} v0.{wide}, v1.{nr}, v2.{ch}[1]", fn, k=2)
    return g

def generate(root, assemble):
    g = build()
    progs = []
    with tempfile.TemporaryDirectory() as wd:
        for i, asm in enumerate(g.progs):
            data, _ = assemble(f"b{i}", asm + "\n  hlt #0", wd)
            progs.append((asm, data))
    h = ["// OTOMATİK ÜRETİLDİ: tools/gen_asimd2.py (assembler + Python oracle; HAFİF test seti). Elle düzenleme.",
         "#pragma once", "#include <cstddef>", "#include <cstdint>", "namespace ncvasimd2 {", ""]
    for i, (_, data) in enumerate(progs): h.append("static const uint8_t p%d[] = {%s};" % (i, ",".join(str(b) for b in data)))
    h.append("struct Prog { const uint8_t* data; size_t len; const char* name; };")
    h.append("static const Prog PROGS[%d] = {" % len(progs))
    for i, (asm, data) in enumerate(progs): h.append('  {p%d, %d, "%s"},' % (i, len(data), asm.replace('"', '')))
    h.append("};")
    h.append("struct Case { uint16_t prog; uint32_t fpcr, fpsr; uint64_t v[6][2]; uint64_t v0[2]; uint64_t out[2]; };")
    q = lambda x: "0x%xull,0x%xull" % (x & M64, (x >> 64) & M64)
    h.append("static const Case CASES[%d] = {" % len(g.cases))
    for pi, fpcr, fpsr, v, d, out in g.cases:
        h.append("  {%d,0x%x,0x%x,{%s},{%s},{%s}}," % (pi, fpcr, fpsr, ",".join("{%s}" % q(x) for x in v), q(d), q(out)))
    h.append("};")
    h.append("static const size_t PROG_COUNT = %d, CASE_COUNT = %d;" % (len(progs), len(g.cases)))
    h.append("}  // namespace ncvasimd2\n")
    out = os.path.join(root, "native-tests", "generated", "test_asimd2.h")
    open(out, "w").write("\n".join(h))
    print(f"{len(progs)} ASIMD2 programı, {len(g.cases)} vaka -> {os.path.getsize(out)//1024} KiB")
