# Regression: initial Android boot stop at MDSCR_EL1

Observed guest stop:

```text
reason=UnsupportedInstruction pc=0x4102365c insn=0xd5100240
opcode=0xd5100240 class=system-register MSR S2_0_C0_C2_2, Rt=X0
```

Decode: `0xD5100240` is `MSR MDSCR_EL1, X0` (`S2_0_C0_C2_2`). It is an architectural AArch64 debug system-register write, not an MTK MMIO access.

Change: the interpreter recognizes the common AArch64 debug sysreg bank and models it RAZ/WI while debug execution features are not implemented. The JNI diagnostic also corrects the op0 display and labels MDSCR_EL1.

Regression: `kernel_mdscr_el1_is_nonfatal_when_debug_architecture_is_disabled` executes the exact `MSR MDSCR_EL1, X0` encoding, reads it back as zero, and reaches HLT.

Host checks in this edit pass:

- Release: 107 tests passed.
- ASan/UBSan: 107 tests passed.
- ThreadSanitizer: 107 tests passed.
- JNI source syntax checked with JDK 21 headers.

Limitations: no Android APK build, physical-device run, or actual kernel boot was performed in this environment. This fixes only the reported first stop. Re-run the original boot attempt to collect the next failure.
