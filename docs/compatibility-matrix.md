# AArch64 komut desteği (Aşama 2c tamamlandı: ASIMD tamsayı + FP vektör + doyurucu/uzun-geniş-dar/by-element/skaler-SIMD)
Kodlamalar `aarch64-linux-gnu-as` çıktısıyla üretildi; beklenen sonuçlar Python'da komutun matematiksel anlamından hesaplandı
(`tools/gen_payloads.py`, `tools/gen_vectors.py`). Veri işleme komutları 64-bit (X) ve 32-bit (W) formda desteklenir.

| Grup | Desteklenen | Desteklenmeyen (UnsupportedInstruction) |
|---|---|---|
| Hint/bariyer | NOP, YIELD, SEV, SEVL, WFE/WFI (yalnızca no-op ipucu), tüm PAC/BTI ve ayrılmış hint'ler (NOP gibi), DMB/DSB/ISB (tek çekirdekte etkisiz), CLREX | SB |
| Sistem | HLT(*), BRK, SVC, seçili MRS/MSR, ERET, EL0/EL1, 4 KiB stage-1 MMU, sınırlı abort exception, CRn=7 cache operation no-op, CRn=8 TLBI full software-TLB flush | Sistem/feature/timer register kapsamı hâlâ kısmi; tam SYS/DC/IC/TLBI semantiği, EL2/EL3, FIQ/SError, tam GIC/PSCI, gerçek Linux/Android boot doğrulaması yok |
| Dallanma | B, BL, B.cond (15 koşul), CBZ/CBNZ (X,W), TBZ/TBNZ, BR, BLR, RET | PAC'li dallanmalar |
| Sabit | MOVN, MOVZ, MOVK | — |
| Aritmetik | ADD/ADDS/SUB/SUBS: immediate, shifted-reg, extended-reg (UXT*/SXT*, SP dahil); ADC/ADCS/SBC/SBCS (NGC); CMP/CMN | — |
| Mantık | AND/ORR/EOR/ANDS + BIC/ORN/EON/BICS (shifted-reg, ROR), mantık-immediate, TST | — |
| Koşullu | CSEL/CSINC/CSINV/CSNEG (CSET/CSETM), CCMP/CCMN (register, immediate) | — |
| Çarpma/bölme | MADD/MSUB (MUL), SMADDL/SMSUBL/UMADDL/UMSUBL, SMULH, UMULH, UDIV, SDIV | — |
| Kaydırma/bitfield | LSLV/LSRV/ASRV/RORV, SBFM/UBFM/BFM (+takma adlar), EXTR (ROR imm) | — |
| Bit işlemleri | RBIT, REV16, REV32, REV, CLZ, CLS | PAC (PACIA.. vb. opcode>=8), CRC32 |
| Adres | ADR, ADRP | — |
| Yükleme/saklama | LDR/STR B/H/W/X: unsigned-offset, unscaled, pre/post-index, register-offset, literal; LDRS{B,H,W}; LDP/STP/LDPSW; LDNP/STNP; LDTR/STTR(**); PRFM/PRFUM (etkisiz) | SIMD/FP yükleme-saklama (V=1) |
| Exclusive / acq-rel | LDXR/STXR/LDAXR/STLXR (B/H/W/X), LDXP/STXP/LDAXP/STLXP, LDAR/STLR (B/H/W/X); yerel monitor; hizasız erişimde AlignmentFault | LDLAR/STLLR, LDAPR (RCpc) |
| LSE atomikler | — | CAS/CASP, LDADD/SWP/... (ID register'larında ilan edilmeyecek) |
| Skaler FP (S/D) | FMOV (reg, imm, GPR<->FP, X<->V.D[1]), FABS, FNEG, FSQRT, FADD/FSUB/FMUL/FDIV/FNMUL, FMAX/FMIN/FMAXNM/FMINNM, FMADD/FMSUB/FNMADD/FNMSUB, FCMP/FCMPE (reg, #0.0), FCCMP/FCCMPE, FCSEL, FRINTN/P/M/Z/A/X/I, FCVT (S/D/H arası), FCVT{N,P,M,Z,A}{S,U} (W/X), SCVTF/UCVTF, sabit-nokta FCVTZ*/SCVTF/UCVTF; FPCR (RMode, FZ, DN), FPSR bayrakları (IOC DZC OFC UFC IXC IDC) | half aritmetiği (FEAT_FP16), BF16, FJCVTZS, FRINT32/64*, FPCR.AHP, tuzak (trap) bitleri |
| SIMD&FP bellek | LDR/STR B/H/S/D/Q: unsigned-offset, unscaled, pre/post-index, register-offset, literal; LDP/STP/LDNP/STNP S/D/Q; **LD1..LD4/ST1..ST4: çoklu yapı (1-4 yazmaç, tüm düzenlemeler), tek-lane, LD1R..LD4R, post-index (immediate ve register)** | — |
| ASIMD tamsayı (2c-2a) | Üç-aynı: ADD/SUB, CMEQ/CMGT/CMGE/CMHI/CMHS/CMTST, AND/BIC/ORR/ORN/EOR/BSL/BIT/BIF, MUL/MLA/MLS/PMUL(8b), SMAX/UMAX/SMIN/UMIN, SABD/UABD/SABA/UABA, SHADD/UHADD/SRHADD/URHADD/SHSUB/UHSUB, SSHL/USHL/SRSHL/URSHL, pairwise ADDP/SMAXP/UMAXP/SMINP/UMINP; İki-register: REV64/32/16, CLS/CLZ/CNT/NOT/RBIT, ABS/NEG, CMxx #0, XTN/XTN2, SHLL/SHLL2, SADDLP/UADDLP/SADALP/UADALP; Across-lanes: ADDV, SADDLV/UADDLV, SMAXV/UMAXV/SMINV/UMINV; DUP/INS/UMOV/SMOV (+skaler MOV Vn.T[i]); MOVI/MVNI/ORR/BIC (immediate); ZIP/UZP/TRN, EXT, TBL/TBX (1-4 yazmaç); immediate kaymalar: SSHR/USHR/SSRA/USRA/SRSHR/URSHR/SRSRA/URSRA/SHL/SLI/SRI, SHRN/RSHRN(+2), SSHLL/USHLL(+2) (SXTL/UXTL); skaler ADDP Dd | (aşağıdaki 2c-2b satırına bakın) |
| ASIMD 2c-2b (HAFİF test) | Doyurucu: SQADD/UQADD, SQSUB/UQSUB, SQSHL/UQSHL (reg+imm), SQRSHL/UQRSHL, SQSHLU, SQDMULH/SQRDMULH, SQABS/SQNEG, SUQADD/USQADD, SQXTN/UQXTN/SQXTUN(+2), SQSHRN/UQSHRN/SQRSHRN/UQRSHRN/SQSHRUN/SQRSHRUN(+2); uzun/geniş/dar: S/UADDL, S/UADDW, S/USUBL, S/USUBW, ADDHN/RADDHN/SUBHN/RSUBHN, S/UABAL, S/UABDL, S/UMLAL, S/UMLSL, S/UMULL (+2 biçimleri), SQDMULL/SQDMLAL/SQDMLSL, PMULL (8b->8h); by-element: MUL/MLA/MLS/SQDMULH/SQRDMULH, S/UMULL/MLAL/MLSL; skaler-SIMD (vektör çekirdeğinin lane-0 modu): üç-aynı, iki-register, kaydırma, by-element, üç-farklı (SQDMULL) | URECPE/URSQRTE, SDOT/UDOT, PMULL.1Q, kripto, FP16 vektör, skaler SQDMLAL/SQDMLSL by-element ayrıntıları test edilmedi |
| ASIMD 2c-3 FP vektör (HAFİF test) | FADD/FSUB/FMUL/FDIV/FABD/FMULX, FMAX/FMIN/FMAXNM/FMINNM, FMLA/FMLS, FRECPS/FRSQRTS, FCMEQ/FCMGE/FCMGT/FACGE/FACGT, pairwise (FADDP/FMAXP/FMINP/FMAXNMP/FMINNMP), FABS/FNEG/FSQRT/FRINT*, FCVT*{S,U}, SCVTF/UCVTF (+sabit-nokta), FCMxx #0.0, FRECPE/FRSQRTE (S/D), FCVTN/FCVTL(+2), FCVTXN, FMAXV/FMINV/FMAXNMV/FMINNMV, skaler pairwise FP, FMOV (vektör imm), FMLA/FMLS/FMUL/FMULX by-element, skaler-SIMD FP formları | FRECPX, FP16 vektör, URECPE, BFloat16, FCMLA/FCADD (complex), FRINT32/64 |
| SVE / kripto | — | tamamı (ilan edilmeyecek) |
| EL0 sistem register'ları | MRS/MSR: NZCV, FPCR, FPSR, TPIDR_EL0, TPIDRRO_EL0 (salt-okunur) — geçici EL0 görünümü | diğer tüm sistem register'ları (Aşama 4) |

(*) HLT: NCV test sözleşmesi; mimari davranış değil.
(**) LDTR/STTR erişim kontrolünün EL0 yetkisini tam uygulamak için ayrı bir erişim seviyesi parametresi hâlâ gerekir.
MMU, 4 KiB granule ve 25–48 bit VA rejimiyle sınırlıdır; TCR’de TG0=4K ve TG1=4K yapılandırması beklenir. 16K/64K, donanım AF/dirty update, contiguous hints, full ID registers ve tam mimari doğrulama henüz yoktur. SCTLR_EL1.A etkinse normal hizasız veri erişimleri alignment abort üretir; hizasız exclusive erişimler şimdilik eski NCV `AlignmentFault` host stop'udur.
UNPREDICTABLE kodlamalar (ör. writeback + Rt==Rn, STXR Rs==Rt) reddedilir.
FP notları: FP işlemleri host FPU'suna değil, saf-tamsayı soft-float'a (`ncv_softfloat.cpp`) dayanır; sonuçlar x86 ve ARM'da aynıdır. FPCR.EnableTrap bitleri RAZ/WI (FP istisnası tuzağa düşmez, yalnızca FPSR'da birikir). FP/SIMD erişimi şimdilik her zaman açıktır (CPACR_EL1.FPEN denetimi Aşama 4'te eklenmeli).
Bu tablo "AArch64 emülatörü tamamlandı" veya "Linux/Android/GSI boot destekleniyor" anlamına GELMEZ. Linux kernel boot için eksik ISA/register davranışları, daha tam GIC/timer/PSCI, depolama ve boot zinciri hâlâ gerekir; mevcut DTB ve raw Image yolu tek başına boot kanıtı değildir.


## Stage 4C/4D additions (limited profile)

| Area | Status | Notes |
|---|---|---|
| GICv2 | Partial | Single vCPU, limited distributor/CPU interface, IDs 0–255; byte accesses supported for IPRIORITYR/ITARGETSR/CPENDSGIR/SPENDSGIR; no SMP or full security/virtualization semantics. |
| Generic timer | Partial | CNTFRQ/CNTPCT/CNTVCT and physical/virtual TVAL/CTL/CVAL subset; host-monotonic 1 MHz counter; IRQs 30/27. |
| Feature ID registers | Partial | Common ID_AA64* subset with conservative values. Not an exhaustive ARM ARM implementation. |
| FDT | Initial | FDT v17 for RAM/CPU/PL011/GICv2/timer/chosen bootargs/optional initrd address; not yet validated by Linux boot. |
| Linux Image + X0 handoff | Host-tested (synthetic) | Raw/gzip ARM64 `Image` header/placement + NCV DTB EL1/X0 setup. No actual Linux boot proof. |
| Android boot image parser | Host-tested (synthetic) | Parses boot.img header versions 0–4 plus vendor_boot v3/v4 and optional init_boot v4; boot.img-only load is accepted when it embeds a non-empty generic ramdisk. No successful Android boot proof. |
| MTK probe fallback | Host-tested | Optional profile returns zero/ignores writes for otherwise-unmapped addresses in the heuristic 0x10000000–0x1fffffff window and reports counts. This is not a Helio G85/MTK SoC emulator or OEM ROM compatibility claim. |
