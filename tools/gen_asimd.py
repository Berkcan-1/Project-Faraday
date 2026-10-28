"""ASIMD (tamsayı vektör) test vektörleri: assembler (kodlama) + lane-tabanlı Python oracle (komutların tanımından)."""
import os, random, tempfile

M64 = (1 << 64) - 1
M128 = (1 << 128) - 1
ARR = {'8b': (0, 8), '16b': (1, 8), '4h': (0, 16), '8h': (1, 16), '2s': (0, 32), '4s': (1, 32), '1d': (0, 64), '2d': (1, 64)}
ALL = ['8b', '16b', '4h', '8h', '2s', '4s', '2d']
NO64 = ['8b', '16b', '4h', '8h', '2s', '4s']
POISON_V0 = 0xDEADBEEFCAFEF00D_A5A5A5A5A5A5A5A5

def mask(b): return (1 << b) - 1
def sx(v, b):
    v &= mask(b)
    return v - (1 << b) if v >> (b - 1) else v
def lanes(v, esz, n): return [(v >> (i * esz)) & mask(esz) for i in range(n)]
def pack(ls, esz):
    r = 0
    for i, x in enumerate(ls): r |= (x & mask(esz)) << (i * esz)
    return r
def geo(arr):
    q, esz = ARR[arr]; return q, esz, (128 if q else 64) // esz
def dmask(arr): return M128 if ARR[arr][0] else M64

class Gen:
    def __init__(self):
        self.progs, self.pidx, self.cases, self.mcases, self.rng = [], {}, [], [], random.Random(4242)
    def prog(self, asm):
        if asm not in self.pidx: self.pidx[asm] = len(self.progs); self.progs.append(asm)
        return self.pidx[asm]
    def rv(self):
        b = []
        for _ in range(16):
            c = self.rng.random()
            b.append(0x00 if c < .1 else 0xFF if c < .2 else 0x80 if c < .3 else 0x7F if c < .4 else 0x01 if c < .45 else self.rng.getrandbits(8))
        return sum(x << (8 * i) for i, x in enumerate(b))
    def emit(self, asm, fn, kind=0, k=8, x1=False, idxreg=None):
        pi = self.prog(asm)
        for _ in range(k):
            v = [self.rv() for _ in range(6)]
            if idxreg:                       # tbl endeksi: tablo boyutuna yakın değerler
                tl = idxreg
                v[4] = sum(((self.rng.randrange(tl * 16 + 6)) << (8 * i)) for i in range(16))
            d = self.rng.choice([self.rv(), POISON_V0])
            xv = self.rng.getrandbits(64)
            out = fn(d, v, xv) & (M128 if kind == 0 else M64)
            self.cases.append((pi, kind, xv, v, d, out))

def three_same(g):
    S, shl = sx, None
    def lanewise(arr, f):
        q, e, n = geo(arr)
        def fn(d, v, x):
            dl, al, bl = lanes(d, e, n), lanes(v[0], e, n), lanes(v[1], e, n)
            return pack([f(a, b, dd, e) for a, b, dd in zip(al, bl, dl)], e) & dmask(arr)
        return fn
    def shf(a, b, e, signed, rnd):
        sh = sx(b, 8); val = sx(a, e) if signed else a
        if sh >= 0: return (val << sh) & mask(e)
        rs = -sh
        return ((val + ((1 << (rs - 1)) if rnd else 0)) >> rs) & mask(e)
    ops = {
        'add': (ALL, lambda a, b, d, e: a + b), 'sub': (ALL, lambda a, b, d, e: a - b),
        'cmeq': (ALL, lambda a, b, d, e: mask(e) if a == b else 0), 'cmtst': (ALL, lambda a, b, d, e: mask(e) if a & b else 0),
        'cmgt': (ALL, lambda a, b, d, e: mask(e) if sx(a, e) > sx(b, e) else 0), 'cmge': (ALL, lambda a, b, d, e: mask(e) if sx(a, e) >= sx(b, e) else 0),
        'cmhi': (ALL, lambda a, b, d, e: mask(e) if a > b else 0), 'cmhs': (ALL, lambda a, b, d, e: mask(e) if a >= b else 0),
        'mul': (NO64, lambda a, b, d, e: a * b), 'mla': (NO64, lambda a, b, d, e: d + a * b), 'mls': (NO64, lambda a, b, d, e: d - a * b),
        'smax': (NO64, lambda a, b, d, e: a if sx(a, e) > sx(b, e) else b), 'smin': (NO64, lambda a, b, d, e: a if sx(a, e) < sx(b, e) else b),
        'umax': (NO64, lambda a, b, d, e: max(a, b)), 'umin': (NO64, lambda a, b, d, e: min(a, b)),
        'sabd': (NO64, lambda a, b, d, e: abs(sx(a, e) - sx(b, e))), 'uabd': (NO64, lambda a, b, d, e: abs(a - b)),
        'saba': (NO64, lambda a, b, d, e: d + abs(sx(a, e) - sx(b, e))), 'uaba': (NO64, lambda a, b, d, e: d + abs(a - b)),
        'shadd': (NO64, lambda a, b, d, e: (sx(a, e) + sx(b, e)) >> 1), 'uhadd': (NO64, lambda a, b, d, e: (a + b) >> 1),
        'srhadd': (NO64, lambda a, b, d, e: (sx(a, e) + sx(b, e) + 1) >> 1), 'urhadd': (NO64, lambda a, b, d, e: (a + b + 1) >> 1),
        'shsub': (NO64, lambda a, b, d, e: (sx(a, e) - sx(b, e)) >> 1), 'uhsub': (NO64, lambda a, b, d, e: (a - b) >> 1),
        'sshl': (ALL, lambda a, b, d, e: shf(a, b, e, True, False)), 'ushl': (ALL, lambda a, b, d, e: shf(a, b, e, False, False)),
        'srshl': (ALL, lambda a, b, d, e: shf(a, b, e, True, True)), 'urshl': (ALL, lambda a, b, d, e: shf(a, b, e, False, True)),
    }
    for name, (arrs, f) in ops.items():
        for arr in arrs:
            g.emit(f"{name} v0.{arr}, v1.{arr}, v2.{arr}", lanewise(arr, f))
    for arr in ('8b', '16b'):
        dm = dmask(arr)
        L = {'and': lambda d, n, m: n & m, 'bic': lambda d, n, m: n & ~m, 'orr': lambda d, n, m: n | m, 'orn': lambda d, n, m: n | ~m,
             'eor': lambda d, n, m: n ^ m, 'bsl': lambda d, n, m: (d & n) | (~d & m), 'bit': lambda d, n, m: (d & ~m) | (n & m),
             'bif': lambda d, n, m: (d & m) | (n & ~m)}
        for name, f in L.items():
            g.emit(f"{name} v0.{arr}, v1.{arr}, v2.{arr}", lambda d, v, x, f=f, dm=dm: f(d & dm, v[0] & dm, v[1] & dm) & dm)
    def pw(arr, f):
        q, e, n = geo(arr)
        def fn(d, v, x):
            a, b = lanes(v[0], e, n), lanes(v[1], e, n)
            cat = []
            for src in (a, b): cat += [f(src[2 * i], src[2 * i + 1], e) for i in range(n // 2)]
            return pack(cat, e) & dmask(arr)
        return fn
    for name, f, arrs in (('addp', lambda a, b, e: a + b, ALL), ('smaxp', lambda a, b, e: a if sx(a, e) > sx(b, e) else b, NO64),
                          ('sminp', lambda a, b, e: a if sx(a, e) < sx(b, e) else b, NO64), ('umaxp', lambda a, b, e: max(a, b), NO64),
                          ('uminp', lambda a, b, e: min(a, b), NO64)):
        for arr in arrs: g.emit(f"{name} v0.{arr}, v1.{arr}, v2.{arr}", pw(arr, f))

def two_reg(g):
    def per(arr, f):
        q, e, n = geo(arr)
        def fn(d, v, x): return pack([f(a, e) for a in lanes(v[0], e, n)], e) & dmask(arr)
        return fn
    def rev(arr, grp):
        q, e, n = geo(arr); k = grp // e
        def fn(d, v, x):
            a = lanes(v[0], e, n)
            return pack([a[(i // k) * k + (k - 1 - i % k)] for i in range(n)], e) & dmask(arr)
        return fn
    for arr in ('8b', '16b', '4h', '8h', '2s', '4s'): g.emit(f"rev64 v0.{arr}, v1.{arr}", rev(arr, 64))
    for arr in ('8b', '16b', '4h', '8h'): g.emit(f"rev32 v0.{arr}, v1.{arr}", rev(arr, 32))
    for arr in ('8b', '16b'): g.emit(f"rev16 v0.{arr}, v1.{arr}", rev(arr, 16))
    def cls(a, e):
        s = format(a & mask(e), f"0{e}b"); n = 0
        for ch in s[1:]:
            if ch == s[0]: n += 1
            else: break
        return n
    for arr in NO64:
        g.emit(f"cls v0.{arr}, v1.{arr}", per(arr, cls))
        g.emit(f"clz v0.{arr}, v1.{arr}", per(arr, lambda a, e: e - (a & mask(e)).bit_length()))
    for arr in ('8b', '16b'):
        g.emit(f"cnt v0.{arr}, v1.{arr}", per(arr, lambda a, e: bin(a).count('1')))
        g.emit(f"not v0.{arr}, v1.{arr}", per(arr, lambda a, e: ~a))
        g.emit(f"rbit v0.{arr}, v1.{arr}", per(arr, lambda a, e: int(format(a, '08b')[::-1], 2)))
    for arr in ALL:
        g.emit(f"abs v0.{arr}, v1.{arr}", per(arr, lambda a, e: abs(sx(a, e))))
        g.emit(f"neg v0.{arr}, v1.{arr}", per(arr, lambda a, e: -sx(a, e)))
        for nm, f in (('cmgt', lambda s: s > 0), ('cmeq', lambda s: s == 0), ('cmlt', lambda s: s < 0), ('cmge', lambda s: s >= 0), ('cmle', lambda s: s <= 0)):
            g.emit(f"{nm} v0.{arr}, v1.{arr}, #0", per(arr, lambda a, e, f=f: mask(e) if f(sx(a, e)) else 0))
    for dst, src, hi in (('8b', '8h', 0), ('4h', '4s', 0), ('2s', '2d', 0), ('16b', '8h', 1), ('8h', '4s', 1), ('4s', '2d', 1)):
        qd, ed, nd = geo(dst); half = 64 // ed
        def fn(d, v, x, ed=ed, half=half, hi=hi):
            nar = [a & mask(ed) for a in lanes(v[0], ed * 2, half)]
            base = (d & M64) if hi else 0
            return base | (pack(nar, ed) << (64 if hi else 0))
        g.emit(f"xtn{'2' if hi else ''} v0.{dst}, v1.{src}", fn)
    for dst, src, sh, hi in (('8h', '8b', 8, 0), ('4s', '4h', 16, 0), ('2d', '2s', 32, 0), ('8h', '16b', 8, 1), ('4s', '8h', 16, 1), ('2d', '4s', 32, 1)):
        es = sh; half = 64 // es
        def fn(d, v, x, es=es, half=half, hi=hi):
            a = lanes(v[0] >> (64 if hi else 0), es, half)
            return pack([(x_ << es) & mask(es * 2) for x_ in a], es * 2)
        g.emit(f"shll{'2' if hi else ''} v0.{dst}, v1.{src}, #{sh}", fn)
    for nm, signed, acc in (('saddlp', True, False), ('uaddlp', False, False), ('sadalp', True, True), ('uadalp', False, True)):
        for dst, src in (('4h', '8b'), ('8h', '16b'), ('2s', '4h'), ('4s', '8h'), ('1d', '2s'), ('2d', '4s')):
            qs, es, ns = geo(src); qd, ed, nd = geo(dst)
            def fn(d, v, x, es=es, ns=ns, ed=ed, signed=signed, acc=acc):
                a = lanes(v[0], es, ns); dl = lanes(d, ed, ns // 2)
                f = (lambda t: sx(t, es)) if signed else (lambda t: t)
                return pack([f(a[2 * i]) + f(a[2 * i + 1]) + (dl[i] if acc else 0) for i in range(ns // 2)], ed)
            g.emit(f"{nm} v0.{dst}, v1.{src}", fn)

def across(g):
    for nm, rg, arr, fn in (
        [('addv', r, a, 'add') for r, a in (('b', '8b'), ('b', '16b'), ('h', '4h'), ('h', '8h'), ('s', '4s'))] +
        [(n_, r, a, k) for n_, k in (('smaxv', 'smax'), ('sminv', 'smin'), ('umaxv', 'umax'), ('uminv', 'umin')) for r, a in (('b', '8b'), ('b', '16b'), ('h', '4h'), ('h', '8h'), ('s', '4s'))] +
        [(n_, r, a, k) for n_, k in (('saddlv', 'sadd'), ('uaddlv', 'uadd')) for r, a in (('h', '8b'), ('h', '16b'), ('s', '4h'), ('s', '8h'), ('d', '4s'))]):
        q, e, n = geo(arr)
        def f(d, v, x, e=e, n=n, k=fn):
            a = lanes(v[0], e, n)
            if k == 'add': return sum(a) & mask(e)
            if k == 'smax': return max(a, key=lambda t: sx(t, e))
            if k == 'smin': return min(a, key=lambda t: sx(t, e))
            if k == 'umax': return max(a)
            if k == 'umin': return min(a)
            if k == 'sadd': return sum(sx(t, e) for t in a) & mask(2 * e)
            return sum(a) & mask(2 * e)
        g.emit(f"{nm} {rg}0, v1.{arr}", f)

def copy_ops(g):
    for arr, idxs in (('16b', (0, 7, 15)), ('8b', (0, 5)), ('8h', (0, 6)), ('4h', (1, 3)), ('4s', (0, 3)), ('2s', (1,)), ('2d', (0, 1))):
        q, e, n = geo(arr)
        for i in idxs:
            ch = {8: 'b', 16: 'h', 32: 's', 64: 'd'}[e]
            g.emit(f"dup v0.{arr}, v1.{ch}[{i}]", lambda d, v, x, e=e, n=n, i=i, arr=arr: pack([lanes(v[0], e, 128 // e)[i]] * n, e) & dmask(arr), k=4)
        rg = 'x' if e == 64 else 'w'
        g.emit(f"dup v0.{arr}, {rg}1", lambda d, v, x, e=e, n=n, arr=arr: pack([x & mask(e)] * n, e) & dmask(arr), k=4)
    for ch, e, n, idxs in (('b', 8, 16, (0, 3, 15)), ('h', 16, 8, (0, 5, 7)), ('s', 32, 4, (0, 2, 3)), ('d', 64, 2, (0, 1))):
        rg = 'x' if e == 64 else 'w'
        for i in idxs:
            def ins(d, v, x, e=e, n=n, i=i):
                l = lanes(d, e, n); l[i] = x & mask(e); return pack(l, e)
            g.emit(f"ins v0.{ch}[{i}], {rg}1", ins, k=4)
            for j in (0, n - 1):
                def inse(d, v, x, e=e, n=n, i=i, j=j):
                    l = lanes(d, e, n); l[i] = lanes(v[0], e, n)[j]; return pack(l, e)
                g.emit(f"mov v0.{ch}[{i}], v1.{ch}[{j}]", inse, k=3)
            g.emit(f"umov {rg}0, v1.{ch}[{i}]", lambda d, v, x, e=e, n=n, i=i: lanes(v[0], e, n)[i], kind=1, k=4)
            g.emit(f"mov d0, v1.d[{i % 2}]" if e == 64 else f"mov {ch}0, v1.{ch}[{i}]",
                   (lambda d, v, x, i=i: lanes(v[0], 64, 2)[i % 2]) if e == 64 else (lambda d, v, x, e=e, n=n, i=i: lanes(v[0], e, n)[i]), k=3)
    for ch, e, n, idxs, dsts in (('b', 8, 16, (0, 7, 15), ('w', 'x')), ('h', 16, 8, (0, 3, 7), ('w', 'x')), ('s', 32, 4, (0, 2), ('x',))):
        for i in idxs:
            for rg in dsts:
                dm = M64 if rg == 'x' else 0xFFFFFFFF
                g.emit(f"smov {rg}0, v1.{ch}[{i}]", lambda d, v, x, e=e, n=n, i=i, dm=dm: sx(lanes(v[0], e, n)[i], e) & dm, kind=1, k=4)
    g.emit("addp d0, v1.2d", lambda d, v, x: (v[0] & M64) + (v[0] >> 64) & M64)

def mod_imm(g):
    IM = (0x00, 0x7F, 0xAB, 0xFF)
    def rep(lane, e, arr): return pack([lane] * geo(arr)[2], e) & dmask(arr)
    for imm in IM:
        for sh in (0, 8, 16, 24):
            for arr in ('4s', '2s'):
                lane = (imm << sh) & 0xFFFFFFFF
                g.emit(f"movi v0.{arr}, #{imm}, lsl #{sh}", lambda d, v, x, lane=lane, arr=arr: rep(lane, 32, arr), k=1)
                g.emit(f"mvni v0.{arr}, #{imm}, lsl #{sh}", lambda d, v, x, lane=lane, arr=arr: rep(~lane & 0xFFFFFFFF, 32, arr), k=1)
                g.emit(f"orr v0.{arr}, #{imm}, lsl #{sh}", lambda d, v, x, lane=lane, arr=arr: pack([l | lane for l in lanes(d, 32, 4)][:geo(arr)[2]], 32) & dmask(arr), k=2)
                g.emit(f"bic v0.{arr}, #{imm}, lsl #{sh}", lambda d, v, x, lane=lane, arr=arr: pack([l & ~lane for l in lanes(d, 32, 4)][:geo(arr)[2]], 32) & dmask(arr), k=2)
        for sh in (0, 8):
            for arr in ('8h', '4h'):
                lane = (imm << sh) & 0xFFFF
                g.emit(f"movi v0.{arr}, #{imm}, lsl #{sh}", lambda d, v, x, lane=lane, arr=arr: rep(lane, 16, arr), k=1)
                g.emit(f"mvni v0.{arr}, #{imm}, lsl #{sh}", lambda d, v, x, lane=lane, arr=arr: rep(~lane & 0xFFFF, 16, arr), k=1)
                g.emit(f"orr v0.{arr}, #{imm}, lsl #{sh}", lambda d, v, x, lane=lane, arr=arr: pack([l | lane for l in lanes(d, 16, 8)][:geo(arr)[2]], 16) & dmask(arr), k=2)
                g.emit(f"bic v0.{arr}, #{imm}, lsl #{sh}", lambda d, v, x, lane=lane, arr=arr: pack([l & ~lane for l in lanes(d, 16, 8)][:geo(arr)[2]], 16) & dmask(arr), k=2)
        for sh in (8, 16):
            for arr in ('4s', '2s'):
                lane = (imm << sh) | mask(sh)
                g.emit(f"movi v0.{arr}, #{imm}, msl #{sh}", lambda d, v, x, lane=lane, arr=arr: rep(lane, 32, arr), k=1)
                g.emit(f"mvni v0.{arr}, #{imm}, msl #{sh}", lambda d, v, x, lane=lane, arr=arr: rep(~lane & 0xFFFFFFFF, 32, arr), k=1)
        for arr in ('16b', '8b'):
            g.emit(f"movi v0.{arr}, #{imm}", lambda d, v, x, imm=imm, arr=arr: rep(imm, 8, arr), k=1)
    for lit in (0xFF00FF00FF00FF00, 0x00000000FFFFFFFF, 0xFF000000000000FF, 0x0):
        g.emit(f"movi v0.2d, #{hex(lit)}", lambda d, v, x, lit=lit: lit | (lit << 64), k=1)
        g.emit(f"movi d0, #{hex(lit)}", lambda d, v, x, lit=lit: lit, k=1)

def permute(g):
    def fn_for(op, arr):
        q, e, n = geo(arr)
        def fn(d, v, x):
            a, b = lanes(v[0], e, n), lanes(v[1], e, n); r = [0] * n; h = n // 2
            if op in ('zip1', 'zip2'):
                base = 0 if op == 'zip1' else h
                for i in range(h): r[2 * i], r[2 * i + 1] = a[base + i], b[base + i]
            elif op in ('uzp1', 'uzp2'):
                par = 0 if op == 'uzp1' else 1; cat = a + b
                r = [cat[2 * i + par] for i in range(n)]
            else:
                par = 0 if op == 'trn1' else 1
                for i in range(h): r[2 * i], r[2 * i + 1] = a[2 * i + par], b[2 * i + par]
            return pack(r, e) & dmask(arr)
        return fn
    for op in ('zip1', 'zip2', 'uzp1', 'uzp2', 'trn1', 'trn2'):
        for arr in ALL: g.emit(f"{op} v0.{arr}, v1.{arr}, v2.{arr}", fn_for(op, arr))
    for arr, idxs in (('16b', (0, 1, 5, 15)), ('8b', (0, 3, 7))):
        nb = 16 if arr == '16b' else 8
        for i in idxs:
            def fn(d, v, x, nb=nb, i=i):
                cat = lanes(v[0], 8, 16) + lanes(v[1], 8, 16)
                if nb == 8: cat = lanes(v[0], 8, 8) + lanes(v[1], 8, 8)
                return pack(cat[i:i + nb], 8)
            g.emit(f"ext v0.{arr}, v1.{arr}, v2.{arr}, #{i}", fn)
    for arr, nb in (('16b', 16), ('8b', 8)):
        for ln in (1, 2, 3, 4):
            regs = ", ".join(f"v{r}.16b" for r in range(1, ln + 1))
            for op in ('tbl', 'tbx'):
                def fn(d, v, x, nb=nb, ln=ln, op=op):
                    tab = sum((lanes(v[r], 8, 16) for r in range(ln)), []); idx = lanes(v[4], 8, 16)
                    base = lanes(d, 8, 16) if op == 'tbx' else [0] * 16
                    out = [tab[idx[i]] if idx[i] < ln * 16 else base[i] for i in range(nb)] + [0] * (16 - nb)
                    return pack(out, 8)
                g.emit(f"{op} v0.{arr}, {{{regs}}}, v5.{arr}", fn, k=10, idxreg=ln)

def shifts(g):
    def lane_fn(arr, f):
        q, e, n = geo(arr)
        def fn(d, v, x):
            return pack([f(a, dd, e) for a, dd in zip(lanes(v[0], e, n), lanes(d, e, n))], e) & dmask(arr)
        return fn
    for arr in ALL:
        q, e, n = geo(arr)
        for sh in sorted({1, e // 2, e}):
            for nm, f in (('sshr', lambda a, d, e, sh=sh: sx(a, e) >> sh), ('ushr', lambda a, d, e, sh=sh: a >> sh),
                          ('ssra', lambda a, d, e, sh=sh: d + (sx(a, e) >> sh)), ('usra', lambda a, d, e, sh=sh: d + (a >> sh)),
                          ('srshr', lambda a, d, e, sh=sh: (sx(a, e) + (1 << (sh - 1))) >> sh), ('urshr', lambda a, d, e, sh=sh: (a + (1 << (sh - 1))) >> sh),
                          ('srsra', lambda a, d, e, sh=sh: d + ((sx(a, e) + (1 << (sh - 1))) >> sh)), ('ursra', lambda a, d, e, sh=sh: d + ((a + (1 << (sh - 1))) >> sh)),
                          ('sri', lambda a, d, e, sh=sh: (d & ~(mask(e) >> sh)) | ((a >> sh) & (mask(e) >> sh)))):
                g.emit(f"{nm} v0.{arr}, v1.{arr}, #{sh}", lane_fn(arr, f), k=5)
        for sh in sorted({0, 1, e - 1}):
            g.emit(f"shl v0.{arr}, v1.{arr}, #{sh}", lane_fn(arr, lambda a, d, e, sh=sh: a << sh), k=5)
            if sh:
                pass
            g.emit(f"sli v0.{arr}, v1.{arr}, #{sh}", lane_fn(arr, lambda a, d, e, sh=sh: (a << sh) | (d & mask(sh))), k=5)
    for nm, rnd in (('shrn', False), ('rshrn', True)):
        for dst, src, hi in (('8b', '8h', 0), ('4h', '4s', 0), ('2s', '2d', 0), ('16b', '8h', 1), ('8h', '4s', 1), ('4s', '2d', 1)):
            qd, ed, nd = geo(dst); half = 64 // ed
            for sh in sorted({1, ed // 2, ed}):
                def fn(d, v, x, ed=ed, half=half, hi=hi, sh=sh, rnd=rnd):
                    nar = [((a + ((1 << (sh - 1)) if rnd else 0)) >> sh) & mask(ed) for a in lanes(v[0], ed * 2, half)]
                    return ((d & M64) if hi else 0) | (pack(nar, ed) << (64 if hi else 0))
                g.emit(f"{nm}{'2' if hi else ''} v0.{dst}, v1.{src}, #{sh}", fn, k=5)
    for nm, signed in (('sshll', True), ('ushll', False)):
        for dst, src, hi in (('8h', '8b', 0), ('4s', '4h', 0), ('2d', '2s', 0), ('8h', '16b', 1), ('4s', '8h', 1), ('2d', '4s', 1)):
            es = {'8h': 8, '4s': 16, '2d': 32}[dst]; half = 64 // es
            for sh in sorted({0, 1, es - 1}):
                def fn(d, v, x, es=es, half=half, hi=hi, sh=sh, signed=signed):
                    a = lanes(v[0] >> (64 if hi else 0), es, half)
                    return pack([((sx(t, es) if signed else t) << sh) & mask(2 * es) for t in a], 2 * es)
                g.emit(f"{nm}{'2' if hi else ''} v0.{dst}, v1.{src}, #{sh}", fn, k=5)

# ---------------- yapı yükleme/saklama (bellek vakaları) ----------------
def structures(g):
    def mem_cases(asm, fn, k=3):
        pi = g.prog(asm)
        for _ in range(k):
            mem = bytes(g.rng.getrandbits(8) for _ in range(64))
            vin = [g.rv() for _ in range(4)]
            x11 = g.rng.choice([0x20, 0x31, 0x40])
            vout, memout, total = fn(bytearray(mem), list(vin), x11)
            g.mcases.append((pi, mem, vin, x11, vout, bytes(memout), total))
    def rd(mem, off, nb): return int.from_bytes(mem[off:off + nb], 'little')
    def wr(mem, off, nb, v): mem[off:off + nb] = (v & mask(nb * 8)).to_bytes(nb, 'little')
    def multi(load, rpt, selem, arr):
        q, e, n = geo(arr); eb = e // 8
        def fn(mem, vin, x11):
            tmp = [0] * (rpt * selem) if load else vin[:rpt * selem]
            tmp = list(tmp); off = 0
            for r in range(rpt):
                for ei in range(n):
                    for s in range(selem):
                        idx = r * selem + s
                        l = lanes(tmp[idx], e, 128 // e)
                        if load: l[ei] = rd(mem, off, eb); tmp[idx] = pack(l, e)
                        else: wr(mem, off, eb, l[ei])
                        off += eb
            vout = list(vin)
            if load:
                for i in range(rpt * selem): vout[i] = tmp[i]
            return vout, mem, off
        return fn
    def single(load, selem, ch, idx, repl, arr=None):
        e = {'b': 8, 'h': 16, 's': 32, 'd': 64}[ch]; eb = e // 8
        def fn(mem, vin, x11):
            vout = list(vin); off = 0
            for s in range(selem):
                if repl:
                    q, ee, n = geo(arr); vout[s] = pack([rd(mem, off, eb)] * n, e)
                elif load:
                    l = lanes(vout[s], e, 128 // e); l[idx] = rd(mem, off, eb); vout[s] = pack(l, e)
                else: wr(mem, off, eb, lanes(vin[s], e, 128 // e)[idx])
                off += eb
            return vout, mem, off
        return fn
    def fmt(regs, arr): return "{" + ", ".join(f"v{r}.{arr}" for r in range(regs)) + "}"
    for mn, load in (('ld', True), ('st', False)):
        for cnt in (1, 2, 3, 4):
            for arr in (ALL + ['1d'] if cnt == 1 else NO64 + ['2d']):
                opc = {1: (cnt, 1), 2: (1, 2), 3: (1, 3), 4: (1, 4)}[cnt]
                for fx, post in (("", ""), (", #{t}", "i"), (", x11", "r")):
                    q, e, n = geo(arr)
                    total = n * (e // 8) * cnt
                    form = f"[x10]" + (fx.format(t=total) if fx else "")
                    # ld1 çok yazmaçlı (cnt=1..4 ardışık yazmaç) ve ld2/3/4 (selem=cnt)
                    for kind_, rpt, selem in ((("ld1" if True else ""), cnt, 1),) if False else ():
                        pass
                    asm = f"{mn}{1} {fmt(cnt, arr)}, {form}"
                    mem_cases(asm, wrapp(multi(load, cnt, 1, arr), post, total), k=2)
                    if cnt >= 2:
                        asm = f"{mn}{cnt} {fmt(cnt, arr)}, {form}"
                        mem_cases(asm, wrapp(multi(load, 1, cnt, arr), post, total), k=2)
    for mn, load in (('ld', True), ('st', False)):
        for selem in (1, 2, 3, 4):
            for ch, idxs in (('b', (0, 5, 15)), ('h', (0, 3, 7)), ('s', (0, 2, 3)), ('d', (0, 1))):
                e = {'b': 8, 'h': 16, 's': 32, 'd': 64}[ch]
                for idx in idxs:
                    for fx, post in (("", ""), (", #{t}", "i"), (", x11", "r")):
                        total = selem * e // 8
                        regs = "{" + ", ".join(f"v{r}.{ch}" for r in range(selem)) + "}"
                        asm = f"{mn}{selem} {regs}[{idx}], [x10]" + (fx.format(t=total) if fx else "")
                        mem_cases(asm, wrapp(single(load, selem, ch, idx, False), post, total), k=2)
    for selem in (1, 2, 3, 4):
        for arr in ALL:
            q, e, n = geo(arr)
            if arr == '1d' or (e == 64 and not q): continue
            ch = {8: 'b', 16: 'h', 32: 's', 64: 'd'}[e]
            for fx, post in (("", ""), (", #{t}", "i")):
                total = selem * e // 8
                regs = "{" + ", ".join(f"v{r}.{arr}" for r in range(selem)) + "}"
                asm = f"ld{selem}r {regs}, [x10]" + (fx.format(t=total) if fx else "")
                mem_cases(asm, wrapp(single(True, selem, ch, 0, True, arr), post, total), k=2)

def wrapp(fn, post, total):
    def f(mem, vin, x11):
        vout, memout, tot = fn(mem, vin, x11)
        delta = 0 if post == "" else (total if post == "i" else x11)
        return vout, memout, delta
    return f

def build():
    g = Gen()
    for f in (three_same, two_reg, across, copy_ops, mod_imm, permute, shifts, structures): f(g)
    return g

def generate(root, assemble):
    g = build()
    progs = []
    with tempfile.TemporaryDirectory() as wd:
        for i, asm in enumerate(g.progs):
            data, _ = assemble(f"as{i}", asm + "\n  hlt #0", wd)
            progs.append((asm, data))
    h = ["// OTOMATİK ÜRETİLDİ: tools/gen_asimd.py (aarch64-linux-gnu-as + lane tabanlı Python oracle). Elle düzenleme.",
         "#pragma once", "#include <cstddef>", "#include <cstdint>", "namespace ncvasimd {", ""]
    for i, (_, data) in enumerate(progs):
        h.append("static const uint8_t p%d[] = {%s};" % (i, ",".join(str(b) for b in data)))
    h.append("struct Prog { const uint8_t* data; size_t len; const char* name; };")
    h.append("static const Prog PROGS[%d] = {" % len(progs))
    for i, (asm, data) in enumerate(progs):
        h.append('  {p%d, %d, "%s"},' % (i, len(data), asm.replace('"', '').replace('\\', '')))
    h.append("};")
    h.append("// kind 0: v0 (128-bit) = out ; kind 1: x0 = out[0]")
    h.append("struct Case { uint16_t prog; uint8_t kind; uint64_t x1; uint64_t v[6][2]; uint64_t v0[2]; uint64_t out[2]; };")
    def q(v): return "0x%xull,0x%xull" % (v & M64, (v >> 64) & M64)
    h.append("static const Case CASES[%d] = {" % len(g.cases))
    for pi, kind, xv, v, d, out in g.cases:
        h.append("  {%d,%d,0x%xull,{%s},{%s},{%s}}," % (pi, kind, xv, ",".join("{%s}" % q(x) for x in v), q(d), q(out)))
    h.append("};")
    h.append("struct MCase { uint16_t prog; uint8_t mem[64]; uint64_t vin[4][2]; uint64_t x11; uint64_t vout[4][2]; uint8_t memout[64]; uint64_t delta; };")
    h.append("static const MCase MCASES[%d] = {" % len(g.mcases))
    for pi, mem, vin, x11, vout, memout, delta in g.mcases:
        h.append("  {%d,{%s},{%s},0x%xull,{%s},{%s},%d}," % (pi, ",".join(map(str, mem)), ",".join("{%s}" % q(x) for x in vin), x11,
                 ",".join("{%s}" % q(x) for x in vout), ",".join(map(str, memout)), delta))
    h.append("};")
    h.append("static const size_t PROG_COUNT = %d, CASE_COUNT = %d, MCASE_COUNT = %d;" % (len(progs), len(g.cases), len(g.mcases)))
    h.append("}  // namespace ncvasimd\n")
    out = os.path.join(root, "native-tests", "generated", "test_asimd.h")
    open(out, "w").write("\n".join(h))
    print(f"{len(progs)} ASIMD programı, {len(g.cases)} register vakası, {len(g.mcases)} bellek vakası -> {os.path.getsize(out)//1024} KiB")
