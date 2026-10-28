"""Veri güdümlü test vektörleri. Kodlama: gerçek assembler. Beklenen sonuç: komutun MATEMATİKSEL anlamından
(Python tamsayıları) üretilir; C++ yorumlayıcıdaki bit mantığından bağımsızdır."""
import itertools, os, tempfile

M64, M32 = (1 << 64) - 1, (1 << 32) - 1
def mk(bits): return (1 << bits) - 1
def sx(v, bits):
    v &= mk(bits)
    return v - (1 << bits) if v >> (bits - 1) else v

VALS = [0, 1, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, M64, 0x123456789ABCDEF0, 0x80000000]
R0_DEFAULT = 0xDEADBEEFCAFEF00D
R34_DEFAULT = (0x0123456789ABCDEF, 0xFEDCBA9876543210)
CONDS = ["eq","ne","cs","cc","mi","pl","vs","vc","hi","ls","ge","lt","gt","le","al"]

def cond_holds(cc, f):
    n, z, c, v = (bool(x) for x in f)
    return {"eq": z, "ne": not z, "cs": c, "cc": not c, "mi": n, "pl": not n, "vs": v, "vc": not v,
            "hi": c and not z, "ls": not (c and not z), "ge": n == v, "lt": n != v,
            "gt": (not z) and n == v, "le": z or n != v, "al": True}[cc]

# Bayraklar: matematiksel tanım (ARM'ın bit formülü değil)
def f_add(a, b, cin, bits):
    m = mk(bits); a &= m; b &= m
    us = a + b + cin; res = us & m
    return res, (res >> (bits - 1) & 1, int(res == 0), int(us > m), int(sx(a, bits) + sx(b, bits) + cin != sx(res, bits)))
def f_sub(a, b, bits):
    m = mk(bits); a &= m; b &= m
    res = (a - b) & m
    lo, hi = -(1 << (bits - 1)), 1 << (bits - 1)
    return res, (res >> (bits - 1) & 1, int(res == 0), int(a >= b), int(not (lo <= sx(a, bits) - sx(b, bits) < hi)))
def f_logic(res, bits):
    return res, (res >> (bits - 1) & 1, int(res == 0), 0, 0)

def shift(v, t, amt, bits):
    m = mk(bits); v &= m
    if t == "lsl": return (v << amt) & m
    if t == "lsr": return v >> amt
    if t == "asr": return (sx(v, bits) >> amt) & m
    return ((v >> amt) | (v << (bits - amt))) & m  # ror

TEMPLATES = []   # (asm, nin, fn, flag_mode)   flag_mode: "cycle" | "all"
def add(asm, fn, nin=2, flags="cycle", values=None):
    TEMPLATES.append((asm, nin, fn, flags, values))

# ---- 64/32-bit aritmetik + mantık (register, shifted-register) ----
for bits, rg in ((64, "x"), (32, "w")):
    m = mk(bits)
    add(f"add {rg}0, {rg}1, {rg}2", lambda r, f, m=m: ((r[1] + r[2]) & m, None))
    add(f"sub {rg}0, {rg}1, {rg}2", lambda r, f, m=m: ((r[1] - r[2]) & m, None))
    add(f"adds {rg}0, {rg}1, {rg}2", lambda r, f, b=bits: f_add(r[1], r[2], 0, b))
    add(f"subs {rg}0, {rg}1, {rg}2", lambda r, f, b=bits: f_sub(r[1], r[2], b))
    add(f"cmp {rg}1, {rg}2", lambda r, f, b=bits: (r[0], f_sub(r[1], r[2], b)[1]))
    add(f"cmn {rg}1, {rg}2", lambda r, f, b=bits: (r[0], f_add(r[1], r[2], 0, b)[1]))
    for t, a in (("lsl", 3), ("lsr", 7), ("asr", 9)):
        add(f"add {rg}0, {rg}1, {rg}2, {t} #{a}", lambda r, f, b=bits, t=t, a=a: ((r[1] + shift(r[2], t, a, b)) & mk(b), None))
        add(f"subs {rg}0, {rg}1, {rg}2, {t} #{a}", lambda r, f, b=bits, t=t, a=a: f_sub(r[1], shift(r[2], t, a, b), b))
    for t, a in (("lsl", 4), ("ror", 5), ("lsr", 1), ("asr", 3)):
        for op in ("and", "orr", "eor", "bic", "orn", "eon", "ands", "bics"):
            def fn(r, f, b=bits, t=t, a=a, op=op):
                m_ = mk(b); x = r[1] & m_; y = shift(r[2], t, a, b)
                if op in ("bic", "orn", "eon", "bics"): y = ~y & m_
                res = {"and": x & y, "ands": x & y, "bic": x & y, "bics": x & y,
                       "orr": x | y, "orn": x | y, "eor": x ^ y, "eon": x ^ y}[op]
                return f_logic(res, b) if op in ("ands", "bics") else (res, None)
            add(f"{op} {rg}0, {rg}1, {rg}2, {t} #{a}", fn)
    add(f"tst {rg}1, {rg}2", lambda r, f, b=bits: (r[0], f_logic(r[1] & r[2] & mk(b), b)[1]))
    # immediate
    add(f"add {rg}0, {rg}1, #0xabc", lambda r, f, m=m: ((r[1] + 0xabc) & m, None))
    add(f"add {rg}0, {rg}1, #0x123, lsl #12", lambda r, f, m=m: ((r[1] + (0x123 << 12)) & m, None))
    add(f"adds {rg}0, {rg}1, #0x7ff", lambda r, f, b=bits: f_add(r[1], 0x7ff, 0, b))
    add(f"subs {rg}0, {rg}1, #1", lambda r, f, b=bits: f_sub(r[1], 1, b), nin=1)
    add(f"subs {rg}0, {rg}1, #0x123, lsl #12", lambda r, f, b=bits: f_sub(r[1], 0x123 << 12, b), nin=1)
    add(f"cmp {rg}1, #5", lambda r, f, b=bits: (r[0], f_sub(r[1], 5, b)[1]), nin=1)
    add(f"cmn {rg}1, #1", lambda r, f, b=bits: (r[0], f_add(r[1], 1, 0, b)[1]), nin=1)

add("movz w0, #0xffff, lsl #16", lambda r, f: (0xFFFF0000, None), nin=0)
add("movn w0, #0x1234", lambda r, f: (~0x1234 & M32, None), nin=0)
add("movk w0, #0x5678, lsl #16", lambda r, f: (((r[0] & M32 & ~(0xFFFF << 16)) | (0x5678 << 16)) & M32, None), nin=0)
add("movk x0, #0xbeef, lsl #32", lambda r, f: (((r[0] & ~(0xFFFF << 32)) | (0xBEEF << 32)) & M64, None), nin=0)

# ---- csel ailesi ----
CS_PAIRS = [(5, 9), (M64, 0x8000000000000000), (0x123456789ABCDEF0, 0x00000000FFFFFFFF)]
for bits, rg, conds in ((64, "x", CONDS), (32, "w", ["eq", "lt", "hi", "vs"])):
    m = mk(bits)
    for cc in conds:
        for op in ("csel", "csinc", "csinv", "csneg"):
            def fn(r, f, cc=cc, op=op, m=m):
                a, b = r[1] & m, r[2] & m
                if cond_holds(cc, f): return a, None
                return {"csel": b, "csinc": (b + 1) & m, "csinv": ~b & m, "csneg": (-b) & m}[op], None
            add(f"{op} {rg}0, {rg}1, {rg}2, {cc}", fn, flags="all", values=CS_PAIRS)
        if cc != "al":
            add(f"cset {rg}0, {cc}", lambda r, f, cc=cc: (1 if cond_holds(cc, f) else 0, None), nin=0, flags="all")
            add(f"csetm {rg}0, {cc}", lambda r, f, cc=cc, m=m: (m if cond_holds(cc, f) else 0, None), nin=0, flags="all")

# ---- çarpma / bölme ----
for bits, rg in ((64, "x"), (32, "w")):
    m = mk(bits)
    add(f"mul {rg}0, {rg}1, {rg}2", lambda r, f, m=m: ((r[1] & m) * (r[2] & m) & m, None))
    add(f"madd {rg}0, {rg}1, {rg}2, {rg}3", lambda r, f, m=m: ((r[3] + (r[1] & m) * (r[2] & m)) & m, None), nin=3, values=VALS[:5])
    add(f"msub {rg}0, {rg}1, {rg}2, {rg}3", lambda r, f, m=m: ((r[3] - (r[1] & m) * (r[2] & m)) & m, None), nin=3, values=VALS[:5])
    add(f"udiv {rg}0, {rg}1, {rg}2", lambda r, f, m=m: (((r[1] & m) // (r[2] & m)) if (r[2] & m) else 0, None))
    def sdiv(r, f, b=bits):
        a, d = sx(r[1], b), sx(r[2], b)
        if d == 0: return 0, None
        q = abs(a) // abs(d)
        if (a < 0) != (d < 0): q = -q
        return q & mk(b), None
    add(f"sdiv {rg}0, {rg}1, {rg}2", sdiv)
    for op in ("lsl", "lsr", "asr", "ror"):
        add(f"{op} {rg}0, {rg}1, {rg}2", lambda r, f, b=bits, op=op: (shift(r[1], op, (r[2] & mk(b)) % b, b), None))
add("smull x0, w1, w2", lambda r, f: (sx(r[1], 32) * sx(r[2], 32) & M64, None))
add("umull x0, w1, w2", lambda r, f: ((r[1] & M32) * (r[2] & M32) & M64, None))
add("smaddl x0, w1, w2, x3", lambda r, f: ((r[3] + sx(r[1], 32) * sx(r[2], 32)) & M64, None), nin=3, values=VALS[:5])
add("umsubl x0, w1, w2, x3", lambda r, f: ((r[3] - (r[1] & M32) * (r[2] & M32)) & M64, None), nin=3, values=VALS[:5])
add("smulh x0, x1, x2", lambda r, f: ((sx(r[1], 64) * sx(r[2], 64)) >> 64 & M64, None))
add("umulh x0, x1, x2", lambda r, f: ((r[1] * r[2]) >> 64 & M64, None))

# ---- bitfield (takma adların anlamı üzerinden) ----
def sxw(v, w, bits): return sx(v & mk(w), w) & mk(bits)
for bits, rg in ((64, "x"), (32, "w")):
    m = mk(bits)
    for lsb, w in ((5, 12), (0, 8), (3, 7), (bits - 8, 8)):
        add(f"ubfx {rg}0, {rg}1, #{lsb}, #{w}", lambda r, f, l=lsb, w=w, m=m: (((r[1] & m) >> l) & mk(w), None), nin=1)
        add(f"sbfx {rg}0, {rg}1, #{lsb}, #{w}", lambda r, f, l=lsb, w=w, b=bits, m=m: (sxw(((r[1] & m) >> l), w, b), None), nin=1)
        add(f"bfxil {rg}0, {rg}1, #{lsb}, #{w}", lambda r, f, l=lsb, w=w, m=m: (((r[0] & m & ~mk(w)) | (((r[1] & m) >> l) & mk(w))) & m, None), nin=1)
    for lsb, w in ((8, 16), (4, 8), (0, 4)):
        add(f"bfi {rg}0, {rg}1, #{lsb}, #{w}", lambda r, f, l=lsb, w=w, m=m: (((r[0] & m & ~(mk(w) << l)) | ((r[1] & mk(w)) << l)) & m, None), nin=1)
        add(f"ubfiz {rg}0, {rg}1, #{lsb}, #{w}", lambda r, f, l=lsb, w=w, m=m: (((r[1] & mk(w)) << l) & m, None), nin=1)
        add(f"sbfiz {rg}0, {rg}1, #{lsb}, #{w}", lambda r, f, l=lsb, w=w, b=bits, m=m: ((sx(r[1] & mk(w), w) << l) & m, None), nin=1)
    add(f"bfc {rg}0, #8, #8", lambda r, f, m=m: (r[0] & m & ~(0xFF << 8), None), nin=0)
    for n in (1, 7, bits - 1):
        add(f"lsl {rg}0, {rg}1, #{n}", lambda r, f, n=n, b=bits: (shift(r[1], "lsl", n, b), None), nin=1)
        add(f"lsr {rg}0, {rg}1, #{n}", lambda r, f, n=n, b=bits: (shift(r[1], "lsr", n, b), None), nin=1)
        add(f"asr {rg}0, {rg}1, #{n}", lambda r, f, n=n, b=bits: (shift(r[1], "asr", n, b), None), nin=1)
add("uxtb w0, w1", lambda r, f: (r[1] & 0xFF, None), nin=1)
add("uxth w0, w1", lambda r, f: (r[1] & 0xFFFF, None), nin=1)
add("sxtb w0, w1", lambda r, f: (sxw(r[1], 8, 32), None), nin=1)
add("sxth w0, w1", lambda r, f: (sxw(r[1], 16, 32), None), nin=1)
add("sxtb x0, w1", lambda r, f: (sxw(r[1], 8, 64), None), nin=1)
add("sxth x0, w1", lambda r, f: (sxw(r[1], 16, 64), None), nin=1)
add("sxtw x0, w1", lambda r, f: (sxw(r[1], 32, 64), None), nin=1)

# ---- mantık immediate: assembler bitmask kodlar, beklenen değer literalin kendisinden ----
IMM64 = [0x1, 0x5555555555555555, 0xAAAAAAAAAAAAAAAA, 0x00FF00FF00FF00FF, 0x0000FFFF0000FFFF, 0x7FFFFFFFFFFFFFFF,
         0xFFFFFFFFFFFFFFFE, 0x8000000000000000, 0x00000000FFFFFFFF, 0xF0F0F0F0F0F0F0F0, 0x0303030303030303,
         0x00000000000000FF, 0x7F00000000000000, 0x8000000000000001, 0x0FFFFFFFFFFFFFF0]
IMM32 = [0x1, 0x55555555, 0xFF00FF00, 0x0000FFFF, 0x7FFFFFFF, 0xFFFFFFFE, 0x80000000, 0xF0F0F0F0, 0x0F0F0F0F, 0x00FF0000, 0x80000001]
for bits, rg, imms in ((64, "x", IMM64), (32, "w", IMM32)):
    for imm in imms:
        for op in ("and", "orr", "eor", "ands"):
            def fn(r, f, op=op, imm=imm, b=bits):
                x = r[1] & mk(b)
                res = {"and": x & imm, "ands": x & imm, "orr": x | imm, "eor": x ^ imm}[op]
                return f_logic(res, b) if op == "ands" else (res, None)
            add(f"{op} {rg}0, {rg}1, #{hex(imm)}", fn, nin=1)
add("orr x0, xzr, #0x00ff00ff00ff00ff", lambda r, f: (0x00FF00FF00FF00FF, None), nin=0)
add("tst x1, #0x8000000000000000", lambda r, f: (r[0], f_logic(r[1] & 0x8000000000000000, 64)[1]), nin=1)
add("tst w1, #0x80000000", lambda r, f: (r[0], f_logic(r[1] & 0x80000000, 32)[1]), nin=1)

# ---- tbz/tbnz/cbz/cbnz dalları (sonuç x0'da 0/1) ----
def branch_prog(op, operand):
    return f"  {op} {operand}, 1f\n  movz x0, #0\n  hlt #0\n1:\n  movz x0, #1\n  hlt #0"
for bit in (0, 1, 7, 31, 32, 63):
    for op in ("tbz", "tbnz"):
        add(branch_prog(op, f"x1, #{bit}"), lambda r, f, bit=bit, op=op: (int(bool((r[1] >> bit) & 1) == (op == "tbnz")), None), nin=1)
for bit in (3, 31):
    for op in ("tbz", "tbnz"):
        add(branch_prog(op, f"w1, #{bit}"), lambda r, f, bit=bit, op=op: (int(bool((r[1] >> bit) & 1) == (op == "tbnz")), None), nin=1)
for op in ("cbz", "cbnz"):
    for rg, m in (("x", M64), ("w", M32)):
        add(branch_prog(op, f"{rg}1"), lambda r, f, op=op, m=m: (int(((r[1] & m) == 0) == (op == "cbz")), None), nin=1)


# ================= Aşama 2b =================
PAIRS6 = [(0, 0), (1, M64), (M64, 1), (0x7FFFFFFFFFFFFFFF, 1), (0x8000000000000000, M64), (0x123456789ABCDEF0, 0x0FEDCBA987654321)]
PAIRS4 = [(5, 5), (1, M64), (0x8000000000000000, 1), (0x7FFFFFFFFFFFFFFF, M64)]
SP0 = 0x40000000 + 0x10000     # Machine::kBase + kRam (testler SP'yi buna ayarlar)

def f_adc(a, b, c, bits):
    return f_add(a, b, c, bits)
def f_sbc(a, b, c, bits):
    m = mk(bits); a &= m; b &= m; bor = 1 - c
    res = (a - b - bor) & m
    lo, hi = -(1 << (bits - 1)), 1 << (bits - 1)
    return res, (res >> (bits - 1) & 1, int(res == 0), int(a >= b + bor), int(not (lo <= sx(a, bits) - sx(b, bits) - bor < hi)))

for bits, rg in ((64, "x"), (32, "w")):
    m = mk(bits)
    add(f"adc {rg}0, {rg}1, {rg}2", lambda r, f, m=m: (((r[1] & m) + (r[2] & m) + f[2]) & m, None), flags="all", values=PAIRS6)
    add(f"sbc {rg}0, {rg}1, {rg}2", lambda r, f, m=m: (((r[1] & m) - (r[2] & m) - (1 - f[2])) & m, None), flags="all", values=PAIRS6)
    add(f"adcs {rg}0, {rg}1, {rg}2", lambda r, f, b=bits: f_adc(r[1], r[2], f[2], b), flags="all", values=PAIRS6)
    add(f"sbcs {rg}0, {rg}1, {rg}2", lambda r, f, b=bits: f_sbc(r[1], r[2], f[2], b), flags="all", values=PAIRS6)
    add(f"ngc {rg}0, {rg}2", lambda r, f, m=m: ((0 - (r[2] & m) - (1 - f[2])) & m, None), flags="all", values=PAIRS6)
    add(f"ngcs {rg}0, {rg}2", lambda r, f, b=bits: f_sbc(0, r[2], f[2], b), flags="all", values=PAIRS6)

def ext(v, kind, sh, bits):
    w = {"b": 8, "h": 16, "w": 32, "x": 64}[kind[-1]]
    x = v & mk(w)
    if kind[0] == "s": x = sx(x, w)
    return (x << sh) & mk(bits)
EXT64 = [("add", "x0, x1, w2, uxtb #2", "uxtb", 2), ("add", "x0, x1, w2, sxtw #3", "sxtw", 3), ("sub", "x0, x1, w2, uxth #1", "uxth", 1),
         ("add", "x0, x1, x2, uxtx #4", "uxtx", 4), ("adds", "x0, x1, w2, sxtb", "sxtb", 0), ("subs", "x0, x1, x2, sxtx #2", "sxtx", 2),
         ("add", "x0, x1, w2, sxth #4", "sxth", 4), ("sub", "x0, x1, w2, uxtw", "uxtw", 0)]
for op, args, kind, sh in EXT64:
    def fn(r, f, op=op, kind=kind, sh=sh):
        o2 = ext(r[2], kind, sh, 64)
        if op in ("add",): return (r[1] + o2) & M64, None
        if op == "sub": return (r[1] - o2) & M64, None
        return (f_add(r[1], o2, 0, 64) if op == "adds" else f_sub(r[1], o2, 64))
    add(f"{op} {args}", fn)
for op, args, kind, sh in (("add", "w0, w1, w2, uxtb #1", "uxtb", 1), ("subs", "w0, w1, w2, sxth #2", "sxth", 2), ("adds", "w0, w1, w2, uxtb", "uxtb", 0), ("sub", "w0, w1, w2, sxtb #3", "sxtb", 3)):
    def fn(r, f, op=op, kind=kind, sh=sh):
        o2 = ext(r[2], kind, sh, 32)
        if op == "add": return (r[1] + o2) & M32, None
        if op == "sub": return (r[1] - o2) & M32, None
        return (f_add(r[1], o2, 0, 32) if op == "adds" else f_sub(r[1], o2, 32))
    add(f"{op} {args}", fn)
add("cmp x1, w2, sxtw", lambda r, f: (r[0], f_sub(r[1], ext(r[2], "sxtw", 0, 64), 64)[1]))
add("cmn x1, w2, uxth #3", lambda r, f: (r[0], f_add(r[1], ext(r[2], "uxth", 3, 64), 0, 64)[1]))
add("cmp w1, w2, sxtb #3", lambda r, f: (r[0], f_sub(r[1], ext(r[2], "sxtb", 3, 32), 32)[1]))
add("add x0, sp, w2, uxtw #2", lambda r, f: ((SP0 + ext(r[2], "uxtw", 2, 64)) & M64, None), nin=2)
add("add sp, x1, w2, uxtb\n  mov x0, sp", lambda r, f: ((r[1] + (r[2] & 0xFF)) & M64, None), nin=2)
add("sub x0, sp, x2, uxtx #1", lambda r, f: ((SP0 - ((r[2] << 1) & M64)) & M64, None), nin=2)

for cc in ("eq", "lt", "hi", "vs", "cs"):
    for op, rg, bits, imm_form, nzcv, imm in (("ccmp", "x", 64, False, 5, None), ("ccmn", "w", 32, False, 10, None),
                                              ("ccmp", "w", 32, True, 3, 7), ("ccmn", "x", 64, True, 12, 31)):
        def fn(r, f, cc=cc, op=op, bits=bits, nzcv=nzcv, imm=imm):
            if cond_holds(cc, f):
                b = imm if imm is not None else r[2]
                fl = f_sub(r[1], b, bits)[1] if op == "ccmp" else f_add(r[1], b, 0, bits)[1]
            else:
                fl = ((nzcv >> 3) & 1, (nzcv >> 2) & 1, (nzcv >> 1) & 1, nzcv & 1)
            return r[0], fl
        operand = f"#{imm}" if imm_form else f"{rg}2"
        add(f"{op} {rg}1, {operand}, #{nzcv}, {cc}", fn, flags="all", values=PAIRS4)

def rbit(v, b): return int(format(v & mk(b), f"0{b}b")[::-1], 2)
def rev(v, b, unit):  # unit: bayt cinsinden grup genişliği
    bs = (v & mk(b)).to_bytes(b // 8, "little")
    out = b"".join(bs[i:i + unit][::-1] for i in range(0, b // 8, unit))
    return int.from_bytes(out, "little")
def clz(v, b): return b - (v & mk(b)).bit_length()
def cls(v, b):
    s = format(v & mk(b), f"0{b}b"); n = 0
    for ch in s[1:]:
        if ch == s[0]: n += 1
        else: break
    return n
V1 = VALS + [0x00FF00FF00FF00FF, 0x0000000100000000, 0xFFFFFFFF00000000, 0x4000000000000000]
for bits, rg in ((64, "x"), (32, "w")):
    add(f"rbit {rg}0, {rg}1", lambda r, f, b=bits: (rbit(r[1], b), None), nin=1, values=V1)
    add(f"rev16 {rg}0, {rg}1", lambda r, f, b=bits: (rev(r[1], b, 2), None), nin=1, values=V1)
    add(f"rev {rg}0, {rg}1", lambda r, f, b=bits: (rev(r[1], b, b // 8), None), nin=1, values=V1)
    add(f"clz {rg}0, {rg}1", lambda r, f, b=bits: (clz(r[1], b), None), nin=1, values=V1)
    add(f"cls {rg}0, {rg}1", lambda r, f, b=bits: (cls(r[1], b), None), nin=1, values=V1)
add("rev32 x0, x1", lambda r, f: (rev(r[1], 64, 4), None), nin=1, values=V1)
for bits, rg in ((64, "x"), (32, "w")):
    for lsb in (0, 1, 4, 13, bits - 1):
        add(f"extr {rg}0, {rg}1, {rg}2, #{lsb}", lambda r, f, l=lsb, b=bits: ((((r[1] & mk(b)) << b | (r[2] & mk(b))) >> l) & mk(b), None))
    add(f"ror {rg}0, {rg}1, #7", lambda r, f, b=bits: (shift(r[1], "ror", 7, b), None), nin=1)

def combos(nin, values):
    values = values or VALS
    if values and isinstance(values[0], tuple): return list(values)   # hazır (a,b) çiftleri
    if nin == 0: return [()]
    if nin == 1: return [(v,) for v in values]
    if nin == 2: return list(itertools.product(values, values))
    return list(itertools.product(values, values, values[:3]))

def fmt(v): return f"0x{v & M64:x}ull"

def generate(root, assemble):
    progs, cases = [], []
    with tempfile.TemporaryDirectory() as wd:
        for pi, (asm, nin, fn, flagmode, values) in enumerate(TEMPLATES):
            src = asm if "hlt" in asm else asm + "\n  hlt #0"
            data, _ = assemble(f"vec{pi}", src, wd)
            progs.append((asm.splitlines()[0].strip(), data))
            k = 0
            for combo in combos(nin, values):
                flagsets = range(16) if flagmode == "all" else [k % 16]
                for fl in flagsets:
                    k += 1
                    f = ((fl >> 3) & 1, (fl >> 2) & 1, (fl >> 1) & 1, fl & 1)
                    r = [R0_DEFAULT, 0, 0, R34_DEFAULT[0], R34_DEFAULT[1]]
                    for i, v in enumerate(combo): r[i + 1] = v & M64
                    if nin >= 3: pass
                    out, nf = fn(r, f)
                    nf = f if nf is None else nf
                    nout = (nf[0] << 3) | (nf[1] << 2) | (nf[2] << 1) | nf[3]
                    cases.append((pi, fl, nout, r, out & M64))
    h = ["// OTOMATİK ÜRETİLDİ: tools/gen_vectors.py (aarch64-linux-gnu-as + Python oracle). Elle düzenleme.",
         "#pragma once", "#include <cstddef>", "#include <cstdint>", "namespace ncvvec {", ""]
    for i, (_, data) in enumerate(progs):
        h.append("static const uint8_t p%d[] = {%s};" % (i, ",".join(str(b) for b in data)))
    h.append("struct Prog { const uint8_t* data; size_t len; const char* name; };")
    h.append("static const Prog PROGS[%d] = {" % len(progs))
    for i, (name, data) in enumerate(progs):
        h.append('  {p%d, %d, "%s"},' % (i, len(data), name.replace('"', '')))
    h.append("};")
    h.append("struct Case { uint16_t prog; uint8_t nzcvIn, nzcvOut; uint64_t in[5]; uint64_t outX0; };")
    h.append("static const Case CASES[%d] = {" % len(cases))
    for pi, fl, nout, r, out in cases:
        h.append("  {%d,%d,%d,{%s},%s}," % (pi, fl, nout, ",".join(fmt(x) for x in r), fmt(out)))
    h.append("};")
    h.append("static const size_t PROG_COUNT = %d, CASE_COUNT = %d;" % (len(progs), len(cases)))
    h.append("}  // namespace ncvvec\n")
    out = os.path.join(root, "native-tests", "generated", "test_vectors.h")
    open(out, "w").write("\n".join(h))
    print(f"{len(progs)} vektör programı, {len(cases)} vaka -> {os.path.getsize(out)//1024} KiB")
