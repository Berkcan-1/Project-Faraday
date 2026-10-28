# NCV v0.1.19 — ID_AA64ISAR3_EL1 system-register fix

## Log diagnosis

The attached runtime log shows the generic diagnostic Linux kernel reaching early boot and printing:

```text
[    0.000000] Linux version 7.3.0-g6b4b543fa187
[    0.000000] Machine model: NCV ARM64 virtual platform
...
reason=UnsupportedInstruction pc=0xffff8000800237a8 insn=0xd5380661
class=system-register MRS S3_0_C0_C6_3, Rt=X1
```

`0xD5380661` is `MRS X1, ID_AA64ISAR3_EL1`, encoded as `S3_0_C0_C6_3`. The register is an architectural read-only ID register documented in Linux's ARM64 system-register definitions and the Arm register reference.

References:
- Linux ARM64 sysreg definitions: https://gbmc.googlesource.com/linux/%2B/f5b4df96ee890a075b3004f913fea8d2143df7ca/arch/arm64/tools/sysreg
- Arm register description: https://df.lth.se/~getz/ARM/SysReg/AArch64-id_aa64isar3_el1.html

## Change

- Added `kID_AA64ISAR3_EL1 = sysKey(1, 0, 0, 6, 3)`.
- Added it to the known, EL1-only ID system register set.
- `MRS` returns zero: the generic NCV CPU advertises no optional features from this register.
- `MSR` remains read-only and takes the architecturally modeled undefined path, consistent with other ID registers.
- Added regression test for `0xD5380661` with a nonzero sentinel in X1; the read must set X1 to zero and continue to the next instruction.
- Bumped app version name to `0.1.19-dev`, `versionCode` to 7, and native engine version to `0.1.19-dev-id-aa64isar3-sysreg-fix-20261028`.

## Validation

- ASan/UBSan native suite: **136 tests passed**.
- Release native suite: **136 tests passed**.
- TSan suite: the full script exceeded the available execution time in this environment; it is not reported as passed.
- Android APK build and real-device test were not run here.

## Scope / important limit

This fixes the exact fatal instruction seen in the **generic NCV diagnostic Linux kernel** run. That log is not an Android boot: the generic kernel is not the Infinix kernel and the boot animation will not appear from the diagnostic kernel. After this fix, rerun the diagnostic guest first and check whether Linux reaches `/init` and performs virtio block I/O. Then rerun the selected Infinix `boot.img` separately and diagnose its own first fault/log. This change alone cannot promise Android boot or a vendor logo.
