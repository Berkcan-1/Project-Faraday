"""Skaler FP test vektörleri: assembler (kodlama) + fpo.py (Fraction tabanlı ARM FP oracle)."""
import os, random, struct, tempfile
import fpo
from fpo import Env, RN, RP, RM, RZ, RA

M64 = (1 << 64) - 1
POISON_LO, POISON_HI = 0xDEADBEEFCAFEF00D, 0xA5A5A5A5A5A5A5A5
FPCRS = [0x0, 0x400000, 0x800000, 0xC00000, 0x1000000, 0x2000000, 0x3000000, 0x1400000]
PFX = {'s': 's', 'd': 'd', 'h': 'h'}
SIGN = {'s': 1 << 31, 'd': 1 << 63}
SPECIAL = {
 's': [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x40000000, 0x3F000000, 0x40490FDB, 0x3EAAAAAB, 0x00000001, 0x007FFFFF,
       0x00800000, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000, 0x7F800001, 0xFFC12345, 0x7F9FFFFF, 0x42280000,
       0x3F8CCCCD, 0xC2C80000, 0x4B000000, 0x4B800001],
 'd': [0, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x4000000000000000, 0x3FE0000000000000, 0x400921FB54442D18,
       0x3FD5555555555555, 1, 0x000FFFFFFFFFFFFF, 0x0010000000000000, 0x7FEFFFFFFFFFFFFF, 0xFFEFFFFFFFFFFFFF, 0x7FF0000000000000,
       0xFFF0000000000000, 0x7FF8000000000000, 0x7FF0000000000001, 0xFFF8123456789ABC, 0x7FF7FFFFFFFFFFFF, 0x4045000000000000,
       0x3FF199999999999A, 0xC059000000000000, 0x4330000000000000, 0x4340000000000001],
 'h': [0x0000, 0x8000, 0x3C00, 0xBC00, 0x0001, 0x03FF, 0x0400, 0x7BFF, 0xFBFF, 0x7C00, 0xFC00, 0x7E00, 0x7C01, 0xFE12, 0x3555, 0x4248],
}
SEL = (0, 1, 2, 3, 5, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 18)   # sNaN ve negatif NaN dahil 16 değer
SEL5 = (0, 2, 3, 7, 11)
INTF = [0.0, -0.0, 0.3, 0.5, 0.7, 1.0, 1.5, 2.5, -0.3, -0.5, -0.7, -1.5, -2.5, 2147483647.0, 2147483648.0, -2147483648.0,
        -2147483649.0, 4294967295.0, 4294967296.0, 9223372036854775807.0, 9223372036854775808.0, -9223372036854775808.0,
        18446744073709551615.0, 1e30, -1e30, 0.49999997, 2.5000002]
INTS = [0, 1, 0xFFFFFFFF, 2, 255, (1 << 24) + 1, (1 << 31) - 1, 1 << 31, (1 << 32) - 1, (1 << 53) + 1, (1 << 63) - 1, 1 << 63,
        (1 << 64) - 1, 0x123456789ABCDEF1, 0xFFFFFFFF80000000]
CONDS = ["eq", "lt", "hi", "vs"]

def fb(f, x): return int.from_bytes(struct.pack({'s': '<f', 'd': '<d', 'h': '<e'}[f], x), 'little')

def cond_holds(cc, fl):
    n, z, c, v = (bool(b) for b in fl)
    return {"eq": z, "ne": not z, "cs": c, "cc": not c, "mi": n, "pl": not n, "vs": v, "vc": not v, "hi": c and not z,
            "ls": not (c and not z), "ge": n == v, "lt": n != v, "gt": (not z) and n == v, "le": z or n != v, "al": True}[cc]

class Gen:
    def __init__(self):
        self.progs, self.pidx, self.cases, self.k = [], {}, [], 0
        self.rng = random.Random(2026)
    def prog(self, asm):
        if asm not in self.pidx:
            self.pidx[asm] = len(self.progs); self.progs.append(asm)
        return self.pidx[asm]
    def rnd(self, f):
        E, F, bias, _ = fpo.P(f)
        s = self.rng.getrandbits(1)
        ef = max(1, min((1 << E) - 2, bias + self.rng.randint(-12, 12))) if self.rng.random() < 0.5 else self.rng.randint(0, (1 << E) - 2)
        return (s << (E + F)) | (ef << F) | self.rng.getrandbits(F)
    def emit(self, asm, kind, oracle, x=(0, 0, 0), v=(0, 0, 0), flags_touch=False, nzcv=None):
        """oracle(env) -> out  veya  (out, nzcvOut)."""
        self.k += 1
        fpcr = FPCRS[self.k % len(FPCRS)]
        nzcv_in = self.k % 16 if nzcv is None else nzcv
        env = Env.from_fpcr(fpcr)
        r = oracle(env)
        out, nz = (r if isinstance(r, tuple) else (r, None))
        self.cases.append((self.prog(asm), kind, nzcv_in, nzcv_in if nz is None else nz, fpcr, env.flags, list(x), list(v), out & M64))

def build():
    g = Gen()
    # ---- 2 kaynaklı aritmetik ----
    BIN = {
        "fadd": lambda f, a, b, e: fpo.addsub(f, a, b, e), "fsub": lambda f, a, b, e: fpo.addsub(f, a, b, e, True),
        "fmul": fpo.mul, "fdiv": fpo.div,
        "fmax": lambda f, a, b, e: fpo.minmax(f, a, b, e, False, False), "fmin": lambda f, a, b, e: fpo.minmax(f, a, b, e, True, False),
        "fmaxnm": lambda f, a, b, e: fpo.minmax(f, a, b, e, False, True), "fminnm": lambda f, a, b, e: fpo.minmax(f, a, b, e, True, True),
        "fnmul": lambda f, a, b, e: fpo.mul(f, a, b, e) ^ SIGN[f],
    }
    for f in 'sd':
        p = PFX[f]; vals = [SPECIAL[f][i] for i in SEL]
        for name, fn in BIN.items():
            asm = f"{name} {p}0, {p}1, {p}2"
            for a in vals:
                for b in vals:
                    g.emit(asm, 0, lambda e, a=a, b=b, fn=fn, f=f: fn(f, a, b, e), v=(a, b, 0))
            if name in ("fadd", "fsub", "fmul", "fdiv"):
                for _ in range(64):
                    a, b = g.rnd(f), g.rnd(f)
                    g.emit(asm, 0, lambda e, a=a, b=b, fn=fn, f=f: fn(f, a, b, e), v=(a, b, 0))
    # ---- 1 kaynaklı ----
    UN = {"fsqrt": lambda f, a, e: fpo.fsqrt(f, a, e), "fabs": lambda f, a, e: a & ~SIGN[f], "fneg": lambda f, a, e: a ^ SIGN[f],
          "fmov": lambda f, a, e: a,
          "frintn": lambda f, a, e: fpo.roundint(f, a, RN, False, e), "frintp": lambda f, a, e: fpo.roundint(f, a, RP, False, e),
          "frintm": lambda f, a, e: fpo.roundint(f, a, RM, False, e), "frintz": lambda f, a, e: fpo.roundint(f, a, RZ, False, e),
          "frinta": lambda f, a, e: fpo.roundint(f, a, RA, False, e),
          "frintx": lambda f, a, e: fpo.roundint(f, a, e.rmode, True, e), "frinti": lambda f, a, e: fpo.roundint(f, a, e.rmode, False, e)}
    FRAC = [0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 0.3, -0.3, 2.7, -2.7, 4503599627370496.5 if False else 1048576.5, 8388607.5, 3.0]
    for f in 'sd':
        p = PFX[f]
        extra = [fb(f, x) for x in FRAC]
        for name, fn in UN.items():
            asm = f"{name} {p}0, {p}1"
            for a in SPECIAL[f] + extra:
                g.emit(asm, 0, lambda e, a=a, fn=fn, f=f: fn(f, a, e), v=(a, 0, 0))
            if name == "fsqrt":
                for _ in range(48):
                    a = g.rnd(f)
                    g.emit(asm, 0, lambda e, a=a, f=f: fpo.fsqrt(f, a, e), v=(a, 0, 0))
    # ---- FMA ailesi ----
    FM = {"fmadd": (0, 0), "fmsub": (0, 1), "fnmadd": (1, 0), "fnmsub": (1, 1)}
    def fma_or(f, n, m, a, name, e):
        o1, o0 = FM[name]
        if o1: a ^= SIGN[f]
        if o1 != o0: n ^= SIGN[f]
        return fpo.fma(f, n, m, a, e)
    for f in 'sd':
        p = PFX[f]; v5 = [SPECIAL[f][i] for i in SEL5]
        for name in FM:
            asm = f"{name} {p}0, {p}1, {p}2, {p}3"
            trip = [(a, b, c) for a in v5 for b in v5 for c in v5] + [tuple(g.rng.choice(SPECIAL[f]) for _ in range(3)) for _ in range(80)] \
                   + [(g.rnd(f), g.rnd(f), g.rnd(f)) for _ in range(40)]
            for n, m, a in trip:
                g.emit(asm, 0, lambda e, n=n, m=m, a=a, name=name, f=f: fma_or(f, n, m, a, name, e), v=(0, n, m) if False else (n, m, a))
    # ---- karşılaştırmalar ----
    for f in 'sd':
        p = PFX[f]; vals = [SPECIAL[f][i] for i in SEL]
        for name, sig in (("fcmp", False), ("fcmpe", True)):
            for a in vals:
                for b in vals:
                    g.emit(f"{name} {p}1, {p}2", 2, lambda e, a=a, b=b, sig=sig, f=f: (0, fpo.compare(f, a, b, e, sig)), v=(a, b, 0))
            for a in SPECIAL[f]:
                g.emit(f"{name} {p}1, #0.0", 2, lambda e, a=a, sig=sig, f=f: (0, fpo.compare(f, a, 0, e, sig)), v=(a, 0, 0))
        v8 = [SPECIAL[f][i] for i in (0, 1, 2, 3, 13, 14, 15, 16)]
        for name, sig in (("fccmp", False), ("fccmpe", True)):
            for ci, cc in enumerate(CONDS):
                imm = (ci * 3 + 1) & 15
                for a in v8:
                    for b in v8:
                        def orc(e, a=a, b=b, sig=sig, cc=cc, imm=imm, f=f, fl=None):
                            return None
                        g.k += 0
                        # koşul bayrakları emit içinde kullanıldığı için özel ekleme
                        g.k += 1
                        fpcr = FPCRS[g.k % len(FPCRS)]; nz_in = g.k % 16
                        env = Env.from_fpcr(fpcr)
                        fl = ((nz_in >> 3) & 1, (nz_in >> 2) & 1, (nz_in >> 1) & 1, nz_in & 1)
                        nz = fpo.compare(f, a, b, env, sig) if cond_holds(cc, fl) else imm
                        g.cases.append((g.prog(f"{name} {p}1, {p}2, #{imm}, {cc}"), 2, nz_in, nz, fpcr, env.flags, [0, 0, 0], [a, b, 0], 0))
        v4 = [SPECIAL[f][i] for i in (2, 3, 6, 11)]
        for cc in CONDS:
            for fl in range(16):
                for a, b in ((v4[0], v4[1]), (v4[2], v4[3])):
                    f4 = ((fl >> 3) & 1, (fl >> 2) & 1, (fl >> 1) & 1, fl & 1)
                    g.k += 1
                    fpcr = FPCRS[g.k % len(FPCRS)]
                    g.cases.append((g.prog(f"fcsel {p}0, {p}1, {p}2, {cc}"), 0, fl, fl, fpcr, 0, [0, 0, 0], [a, b, 0], a if cond_holds(cc, f4) else b))
    # ---- FCVT (FP <-> FP) ----
    for src, dst in (('s', 'd'), ('s', 'h'), ('d', 's'), ('d', 'h'), ('h', 's'), ('h', 'd')):
        asm = f"fcvt {PFX[dst]}0, {PFX[src]}1"
        vals = SPECIAL[src] + ([g.rnd(src) for _ in range(48)] if src != 'h' else [g.rnd('h') for _ in range(32)])
        for a in vals:
            g.emit(asm, 0, lambda e, a=a, src=src, dst=dst: fpo.convert(src, dst, a, e), v=(a, 0, 0))
    # ---- FP -> tamsayı ----
    MODES = {"n": RN, "p": RP, "m": RM, "z": RZ, "a": RA}
    for f in 'sd':
        p = PFX[f]
        vals = [fb(f, x) for x in INTF] + [SPECIAL[f][i] for i in (13, 14, 15, 16, 8, 11)]
        for mn, rm in MODES.items():
            for sg, signed in (("s", True), ("u", False)):
                for rg, width in (("w", 32), ("x", 64)):
                    asm = f"fcvt{mn}{sg} {rg}0, {p}1"
                    for a in vals:
                        g.emit(asm, 1, lambda e, a=a, f=f, signed=signed, width=width, rm=rm: fpo.toint(f, a, signed, width, 0, rm, e), v=(a, 0, 0))
        for sg, signed in (("s", True), ("u", False)):
            for rg, width, fbl in (("w", 32, (1, 16, 32)), ("x", 64, (1, 17, 64))):
                for fbits in fbl:
                    asm = f"fcvtz{sg} {rg}0, {p}1, #{fbits}"
                    for a in vals[:20]:
                        g.emit(asm, 1, lambda e, a=a, f=f, signed=signed, width=width, fbits=fbits: fpo.toint(f, a, signed, width, fbits, RZ, e), v=(a, 0, 0))
    # ---- tamsayı -> FP ----
    for f in 'sd':
        p = PFX[f]
        for sg, signed in (("s", True), ("u", False)):
            for rg, width in (("w", 32), ("x", 64)):
                for fbits in (0,) + ((1, 16, 32) if width == 32 else (1, 17, 63, 64)):
                    asm = f"{sg}cvtf {p}0, {rg}1" + (f", #{fbits}" if fbits else "")
                    for iv in INTS:
                        g.emit(asm, 0, lambda e, iv=iv, f=f, signed=signed, width=width, fbits=fbits: fpo.fromint(f, iv & ((1 << width) - 1), signed, width, fbits, e), x=(iv, 0, 0))
    # ---- FMOV ----
    for f, rg in (('s', 'w'), ('d', 'x')):
        mask = (1 << 32) - 1 if f == 's' else M64
        for _ in range(12):
            bits = g.rnd(f)
            g.emit(f"fmov {PFX[f]}0, {rg}1", 0, lambda e, b=bits: b, x=(b if False else bits | (g.rng.getrandbits(32) << 32 if f == 's' else 0), 0, 0))
            g.emit(f"fmov {rg}0, {PFX[f]}1", 1, lambda e, b=bits: b, v=(bits, 0, 0))
    IMMS = [1.0, 2.0, 0.5, -1.5, 31.0, 0.125, 3.0, -0.25, 1.9375, -0.1875, 10.0, -31.0, 0.5625]
    for f in 'sd':
        for x in IMMS:
            g.emit(f"fmov {PFX[f]}0, #{x}", 0, lambda e, x=x, f=f: fb(f, x))
    g.emit("fmov x0, v1.d[1]", 1, lambda e: POISON_HI, v=(0, 0, 0))
    g.emit("fmov v0.d[1], x1", 3, lambda e: 0x0123456789ABCDEF, x=(0x0123456789ABCDEF, 0, 0))
    return g

def generate(root, assemble):
    g = build()
    progs = []
    with tempfile.TemporaryDirectory() as wd:
        for i, asm in enumerate(g.progs):
            src = asm if "hlt" in asm else asm + "\n  hlt #0"
            data, _ = assemble(f"fp{i}", src, wd)
            progs.append((asm, data))
    h = ["// OTOMATİK ÜRETİLDİ: tools/gen_fpvectors.py (aarch64-linux-gnu-as + tools/fpo.py oracle). Elle düzenleme.",
         "#pragma once", "#include <cstddef>", "#include <cstdint>", "namespace ncvfp {", ""]
    for i, (_, data) in enumerate(progs):
        h.append("static const uint8_t p%d[] = {%s};" % (i, ",".join(str(b) for b in data)))
    h.append("struct Prog { const uint8_t* data; size_t len; const char* name; };")
    h.append("static const Prog PROGS[%d] = {" % len(progs))
    for i, (asm, data) in enumerate(progs):
        h.append('  {p%d, %d, "%s"},' % (i, len(data), asm.splitlines()[0].strip().replace('"', "")))
    h.append("};")
    h.append("// kind: 0 v0.lo=out (v0.hi=0), 1 x0=out, 2 yalnız NZCV, 3 v0.lo=poison ve v0.hi=out")
    h.append("struct Case { uint16_t prog; uint8_t kind, nzcvIn, nzcvOut; uint32_t fpcr, fpsr; uint64_t x[3]; uint64_t v[3]; uint64_t out; };")
    h.append("static const Case CASES[%d] = {" % len(g.cases))
    for pi, kind, nzin, nzout, fpcr, fpsr, x, v, out in g.cases:
        h.append("  {%d,%d,%d,%d,0x%x,0x%x,{%s},{%s},0x%xull}," % (pi, kind, nzin, nzout, fpcr, fpsr,
                 ",".join("0x%xull" % (a & M64) for a in x), ",".join("0x%xull" % (a & M64) for a in v), out))
    h.append("};")
    h.append("static const size_t PROG_COUNT = %d, CASE_COUNT = %d;" % (len(progs), len(g.cases)))
    h.append("static const uint64_t POISON_LO = 0x%xull, POISON_HI = 0x%xull;" % (POISON_LO, POISON_HI))
    h.append("}  // namespace ncvfp\n")
    out = os.path.join(root, "native-tests", "generated", "test_fpvectors.h")
    open(out, "w").write("\n".join(h))
    print(f"{len(progs)} FP programı, {len(g.cases)} FP vakası -> {os.path.getsize(out)//1024} KiB")
