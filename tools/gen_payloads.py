#!/usr/bin/env python3
"""NCV test payload üretici.
Test programlarını GERÇEK bir AArch64 assembler'ı (aarch64-linux-gnu-as) ile derler; böylece komut
kodlamaları bizim decoder'ımızdan BAĞIMSIZ üretilir. Çıktı başlıklar repoya eklenir, bu yüzden testleri
çalıştırmak için cross-toolchain gerekmez (yalnızca payload'ları yeniden üretirken gerekir).

Kullanım:  python3 tools/gen_payloads.py
"""
import itertools, os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AS, OBJCOPY, OBJDUMP = "aarch64-linux-gnu-as", "aarch64-linux-gnu-objcopy", "aarch64-linux-gnu-objdump"

HELLO = r'''
  adr  x1, msg
  movz x2, #0x0900, lsl #16
loop:
  ldrb w0, [x1]
  cbz  x0, done
  strb w0, [x2]
  add  x1, x1, #1
  b    loop
done:
  hlt  #0
msg:
  .asciz "NCV GUEST BOOT OK\n"
'''

PROGRAMS = {
 "hello": HELLO,
 "movewide": '''
  movz x0, #0x1234
  movk x0, #0x5678, lsl #16
  movk x0, #0x9abc, lsl #32
  movk x0, #0xdef0, lsl #48
  movn x1, #0
  movz x2, #1, lsl #48
  movn x3, #0x8000, lsl #48
  hlt #0''',
 "addsub_imm": '''
  movz x0, #10
  add  x1, x0, #5
  sub  x2, x0, #3
  add  x3, x0, #1, lsl #12
  subs x4, x0, #10
  hlt #0''',
 "flags_carry_add": '''
  movn x0, #0
  adds x1, x0, #1
  hlt #0''',
 "flags_overflow_add": '''
  movn x0, #0x8000, lsl #48
  adds x1, x0, #1
  hlt #0''',
 "flags_borrow": '''
  movz x0, #3
  subs x1, x0, #5
  hlt #0''',
 "sp_ops": '''
  add  sp, sp, #16
  sub  sp, sp, #32
  add  x5, sp, #0
  hlt #0''',
 "cmp_sp_xzr": '''
  cmp  sp, #0
  hlt #0''',
 "logic_shift": '''
  movz x0, #0xF0
  movz x1, #0x3C
  and  x2, x0, x1
  orr  x3, x0, x1
  eor  x4, x0, x1
  add  x5, x0, x1, lsl #4
  bic  x6, x0, x1
  mov  x7, x1
  ands x8, x0, x1
  movz x9, #0x8000, lsl #48
  add  x10, x0, x9, asr #63
  add  x11, x0, x9, lsr #63
  orr  x12, xzr, x1, ror #4
  hlt #0''',
 "branches_loop": '''
  movz x0, #5
  movz x1, #0
loop:
  add  x1, x1, x0
  subs x0, x0, #1
  b.ne loop
  hlt #0''',
 "call_ret": '''
  movz x0, #0
  bl   func
  hlt #0
func:
  add  x0, x0, #7
  ret''',
 "indirect": '''
  adr  x2, target
  blr  x2
  hlt #0
target:
  movz x3, #0x55
  br   x30''',
 "cbz_cbnz": '''
  movz x0, #1
  cbnz x0, skip
  movz x1, #0xBAD
skip:
  movz x2, #2
  cbz  x0, bad
  movz x3, #3
  hlt #0
bad:
  movz x3, #0xBAD
  hlt #0''',
 "loadstore": '''
  movz x0, #0xBEEF
  movk x0, #0xDEAD, lsl #16
  str  x0, [x1]
  ldr  x2, [x1]
  ldrb w3, [x1]
  ldrb w4, [x1, #1]
  strb w4, [x1, #8]
  ldr  w5, [x1, #4]
  ldr  w6, [x1]
  str  w6, [x1, #16]
  ldr  x7, [x1, #16]
  strh w0, [x1, #32]
  ldrh w8, [x1, #32]
  str  x0, [x1, #0x40]
  ldr  x9, [x1, #0x40]
  hlt #0''',
 "fault_load": '''
  movz x1, #0x1000
  movz x0, #7
  ldr  x0, [x1]
  hlt #0''',
 "fault_store": '''
  movz x1, #0x1000
  movz x0, #7
  str  x0, [x1]
  hlt #0''',
 "fault_mmio_unimpl": '''
  movz x1, #0x0900, lsl #16
  movz x0, #7
  ldr  x0, [x1, #0x100]
  hlt #0''',
 "br_x0": '''
  br   x0
  hlt #0''',
 "xzr_write": '''
  movz xzr, #5
  orr  x0, xzr, xzr
  movz x1, #9
  orr  x2, x1, xzr
  hlt #0''',
 "hlt_imm": "  hlt #0x42",
 "brk_imm": "  brk #0x123",
 "loop_forever": '''
loop:
  b loop''',
 "ls_modes": '''
  movz x1, #0x1111
  movk x1, #0x2222, lsl #16
  movk x1, #0x3333, lsl #32
  movk x1, #0x4444, lsl #48
  movn x2, #0
  stp  x1, x2, [x10]
  ldp  x3, x4, [x10]
  stp  x2, x1, [x10, #16]!
  ldp  x5, x6, [x10]
  ldp  x7, x8, [x10], #16
  stp  w1, w2, [x10]
  ldp  w11, w12, [x10]
  ldpsw x13, x14, [x10]
  stur x1, [x10, #-8]
  ldur x15, [x10, #-8]
  str  x2, [x10, #8]!
  ldr  x16, [x10], #-8
  movz x17, #2
  strb w1, [x10, x17]
  ldrb w18, [x10, x17]
  movz x19, #1
  str  w1, [x10, x19, lsl #2]
  ldr  w20, [x10, x19, lsl #2]
  ldrsw x22, [x10, w19, sxtw #2]
  ldrsb x23, [x10, #4]
  ldrsb x24, [x10, #8]
  ldrsb w25, [x10, #8]
  ldrsh x26, [x10, #8]
  ldrsw x27, [x10, #8]
  movz x29, #0x29
  movz x30, #0x30
  stp  x29, x30, [sp, #-16]!
  movz x29, #0
  movz x30, #0
  ldp  x29, x30, [sp], #16
  hlt #0''',
 "ls_literal": '''
  ldr   x1, lit64
  ldr   w2, lit32
  ldrsw x3, lit32s
  hlt #0
  .balign 8
lit64:  .quad 0x1122334455667788
lit32:  .word 0x99AABBCC
lit32s: .word 0x80000001''',
 "unsup_wfi": "  wfi",
 "unsup_wfe": "  wfe",
 "unsup_ldadd": "  .arch armv8.1-a\n  ldadd x0, x1, [x2]",
 "unsup_cas": "  .arch armv8.1-a\n  cas x0, x1, [x2]",
 "unsup_stxr_unpred": "  .inst 0xC8017C41",
 "excl_unaligned": "  ldxr x0, [x1]",
 "ldar_unaligned": "  ldar x0, [x1]",
 "hints": '''
  nop
  yield
  sev
  sevl
  dmb ish
  dsb sy
  isb
  clrex
  hint #25
  hint #34
  hint #127
  movz x1, #1
  hlt #0''',
 "prfm_nop": '''
  prfm  pldl1keep, [x0]
  prfm  pstl1strm, [x0, #8]
  prfum pldl1keep, [x0, #-1]
  prfm  pldl1keep, [x0, x2]
  prfm  pldl1keep, lit
  movz  x1, #1
  hlt #0
lit: .word 0''',
 "ls_extra": '''
  movz x1, #0x1111
  movk x1, #0x2222, lsl #16
  movn x2, #0
  stnp x1, x2, [x10]
  ldnp x3, x4, [x10]
  stnp w1, w2, [x10, #32]
  ldnp w5, w6, [x10, #32]
  sttr x1, [x10, #48]
  ldtr x7, [x10, #48]
  sttrb w2, [x10, #56]
  ldtrsb x8, [x10, #56]
  ldtrsb w9, [x10, #56]
  sttrh w1, [x10, #58]
  ldtrh w11, [x10, #58]
  ldtrsh x12, [x10, #58]
  ldtrsw x13, [x10, #36]
  hlt #0''',
 "excl": '''
  movz x1, #0x55
  ldxr  x2, [x10]
  add   x2, x2, #1
  stxr  w3, x2, [x10]
  stxr  w26, x2, [x10]
  ldxr  x4, [x10]
  clrex
  stxr  w5, x4, [x10]
  ldaxr x6, [x10]
  stlxr w7, x1, [x10]
  ldar  x8, [x10]
  add   x11, x10, #8
  movz  x9, #0x77
  stlr  x9, [x11]
  ldar  x12, [x11]
  ldxrb w13, [x10]
  stxrb w14, w1, [x10]
  ldaxrh w15, [x10]
  stlxrh w16, w9, [x10]
  ldarb w17, [x10]
  ldarh w18, [x10]
  add   x19, x10, #16
  stp   x1, x9, [x19]
  ldxp  x20, x21, [x19]
  stxp  w22, x9, x1, [x19]
  ldp   x23, x24, [x19]
  stxr  w25, x1, [x10]
  hlt #0''',
 "fp_ldst": '''
  movz x1, #0x1111
  movk x1, #0x2222, lsl #16
  movk x1, #0x3333, lsl #32
  movk x1, #0x4444, lsl #48
  fmov d0, x1
  fmov v0.d[1], x1
  str  q0, [x10]
  ldr  q1, [x10]
  str  d0, [x10, #16]
  ldr  d2, [x10, #16]
  str  s0, [x10, #24]
  ldr  s3, [x10, #24]
  str  h0, [x10, #28]
  ldr  h4, [x10, #28]
  str  b0, [x10, #30]
  ldr  b5, [x10, #30]
  stp  q0, q1, [x10, #64]
  ldp  q6, q7, [x10, #64]
  stp  d0, d2, [x10, #96]!
  ldp  d8, d9, [x10], #16
  ldr  q10, [x10, #-48]
  ldur d11, [x10, #-16]
  movz x12, #2
  str  s0, [x10, x12, lsl #2]
  ldr  s13, [x10, x12, lsl #2]
  ldr  q14, lit
  stnp s0, s0, [x10, #200]
  ldnp s15, s16, [x10, #200]
  hlt #0
  .balign 4
lit: .quad 0x1122334455667788, 0x99AABBCCDDEEFF00''',
 "sys_fp": '''
  mrs  x1, fpcr
  movz x2, #0x0300, lsl #16
  orr  x2, x2, #0x800000
  msr  fpcr, x2
  mrs  x3, fpcr
  movn x4, #0
  msr  fpcr, x4
  mrs  x5, fpcr
  msr  fpsr, x4
  mrs  x6, fpsr
  mrs  x7, nzcv
  movz x8, #0xA000, lsl #16
  msr  nzcv, x8
  mrs  x9, nzcv
  movz x10, #0x1234
  msr  tpidr_el0, x10
  mrs  x11, tpidr_el0
  mrs  x12, tpidrro_el0
  hlt #0''',
 "unsup_msr_ro": "  msr tpidrro_el0, x0",
 "unsup_svc": "  svc #0",
 "unsup_mrs": "  mrs x0, midr_el1",
 "unsup_udf": "  udf #0",
}

CONDS = ["eq","ne","cs","cc","mi","pl","vs","vc","hi","ls","ge","lt","gt","le","al"]
for cc in CONDS:
    PROGRAMS["cond_" + cc] = f'''
  cmp  x0, x1
  b.{cc} 1f
  movz x2, #0
  hlt #0
1:
  movz x2, #1
  hlt #0'''

M = (1 << 64) - 1
def s64(u): return u - (1 << 64) if u >> 63 else u

def expected_taken(cc, a, b):
    """Bağımsız oracle: koşulu bayraklardan değil, matematiksel anlamından üretir (a - b karşılaştırması)."""
    ua, ub = a & M, b & M
    sa, sb = s64(ua), s64(ub)
    wrapped = (ua - ub) & M
    n = bool(wrapped >> 63)
    v = not (-(1 << 63) <= sa - sb < (1 << 63))
    return {"eq": ua == ub, "ne": ua != ub, "cs": ua >= ub, "cc": ua < ub, "mi": n, "pl": not n,
            "vs": v, "vc": not v, "hi": ua > ub, "ls": ua <= ub, "ge": sa >= sb, "lt": sa < sb,
            "gt": sa > sb, "le": sa <= sb, "al": True}[cc]

VALUES = [0, 1, 2, 5, -1, -2, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0x8000000000000001, 0x100000000, 0xFFFFFFFF]

def assemble(name, src, workdir):
    s, o, b = (os.path.join(workdir, name + ext) for ext in (".s", ".o", ".bin"))
    with open(s, "w") as f:
        f.write(".text\n" + src + "\n")
    subprocess.run([AS, "-o", o, s], check=True)
    rel = subprocess.run([OBJDUMP, "-r", o], capture_output=True, text=True, check=True).stdout
    if re.search(r"^\s*[0-9a-f]+\s+R_AARCH64", rel, re.M):
        sys.exit(f"{name}: relocation içeriyor; payload konumdan bağımsız olmalı")
    subprocess.run([OBJCOPY, "-O", "binary", "-j", ".text", o, b], check=True)
    dis = subprocess.run([OBJDUMP, "-d", o], capture_output=True, text=True, check=True).stdout
    return open(b, "rb").read(), dis

def c_array(name, data):
    body = ", ".join(f"0x{x:02x}" for x in data)
    return f"static const uint8_t {name}[] = {{{body}}};\nstatic const size_t {name}_len = {len(data)};\n"

def main():
    import gen_vectors
    out_tests = os.path.join(ROOT, "native-tests", "generated")
    out_app = os.path.join(ROOT, "app", "src", "main", "cpp", "selftest")
    os.makedirs(out_tests, exist_ok=True)
    bins, listing = {}, []
    with tempfile.TemporaryDirectory() as wd:
        for name, src in PROGRAMS.items():
            bins[name], dis = assemble(name, src, wd)
            listing.append(f"=== {name} ===\n{dis}")
    hdr = ["// OTOMATİK ÜRETİLDİ: tools/gen_payloads.py (aarch64-linux-gnu-as). Elle düzenleme.",
           "#pragma once", "#include <cstddef>", "#include <cstdint>", "namespace ncvgen {", ""]
    for name, data in bins.items():
        hdr.append(c_array("prog_" + name, data))
    hdr.append("struct Prog { const uint8_t* data; size_t len; };")
    hdr.append("static const Prog COND_PROGS[%d] = {%s};" % (len(CONDS), ", ".join(f"{{prog_cond_{c}, prog_cond_{c}_len}}" for c in CONDS)))
    hdr.append("static const char* const COND_NAMES[%d] = {%s};" % (len(CONDS), ", ".join(f'"{c}"' for c in CONDS)))
    hdr.append("struct CondVec { uint8_t cc; uint64_t a, b; uint8_t taken; };")
    vecs = [f"{{{i}, 0x{a & M:x}ull, 0x{b & M:x}ull, {1 if expected_taken(cc, a, b) else 0}}}"
            for i, cc in enumerate(CONDS) for a, b in itertools.product(VALUES, VALUES)]
    hdr.append("static const CondVec COND_VECS[] = {%s};" % ",\n  ".join(vecs))
    hdr.append("static const size_t COND_VECS_LEN = %d;" % len(vecs))
    hdr.append("}  // namespace ncvgen\n")
    open(os.path.join(out_tests, "test_programs.h"), "w").write("\n".join(hdr))
    open(os.path.join(out_tests, "test_programs.lst"), "w").write("\n".join(listing))
    app = ["// OTOMATİK ÜRETİLDİ: tools/gen_payloads.py. Kaynak: hello programı (bkz. script). Elle düzenleme.",
           "#pragma once", "#include <cstddef>", "#include <cstdint>", "namespace ncv {",
           c_array("kHelloPayload", bins["hello"]).replace("static const", "static const"),
           "}  // namespace ncv\n"]
    open(os.path.join(out_app, "ncv_payload_hello.h"), "w").write("\n".join(app))
    print(f"{len(PROGRAMS)} program, {len(vecs)} koşul vektörü üretildi")
    gen_vectors.generate(ROOT, assemble)
    import gen_fpvectors, fpo
    fpo.selfcheck(1500)
    gen_fpvectors.generate(ROOT, assemble)
    import gen_asimd
    gen_asimd.generate(ROOT, assemble)
    import gen_asimd2
    gen_asimd2.generate(ROOT, assemble)

if __name__ == "__main__":
    main()
