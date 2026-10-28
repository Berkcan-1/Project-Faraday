# NCV Hybrid DTB / MTK boot-loop diagnostic patch report

Date: 2026-10-27
Engine ID: `0.1.3-dev-hybrid-dtb-20261027`

## Observed failure

The supplied runtime snapshots repeatedly showed `pc=0xffffff80080894a8` and `last_insn=0x14000000` while the retired-instruction counter increased. `0x14000000` decodes to AArch64 `B .`, an unconditional branch to the same address. This is a guest spin/park loop, not an `UnsupportedInstruction` fault. Without the matching `vmlinux`/symbols, the exact kernel function and reason cannot be proven from this PC alone.

## Changes implemented

1. **Embedded Android boot v2 DTB is used in MTK probe profile.** The boot parser extracts the v2 `dtb` payload; a hybrid DTB transformer updates guest RAM, chosen bootargs/stdout, GICv2, timer, PL011, PL031 RTC, virtio block/RNG nodes, simple framebuffer, and disables secondary CPUs. Source vendor SoC nodes are retained for diagnostic probing.
2. **Stale physical memory metadata is removed.** Physical `/reserved-memory` children and FDT reserve-map entries that overlap NCV guest RAM are removed; `memory-region` references are cleared because those physical pools do not exist on the virtual board.
3. **Framebuffer/MTK probe address collision fixed.** The framebuffer previously occupied `0x10000000`, inside the MTK probe fallback's `0x10000000..0x20000000` aperture. Bus accesses in that region could hit the framebuffer device before reaching the probe shim. Framebuffer base moved to `0x3F000000`, below guest RAM (`0x40000000`) and outside the probe aperture.
4. **Live MTK probe sampling added.** UI now prints the MTK fallback counters/recent trace every five seconds while the vCPU runs. Previously the counts were only printed once at VM startup, so `reads=0/writes=0` could merely be the pre-run report.
5. **Build identity and runtime diagnostics.** Engine ID is `0.1.3-dev-hybrid-dtb-20261027`. Runtime snapshots include X0-X3, `SCTLR_EL1`, `ESR_EL1`, `FAR_EL1`, EL, repeated-PC slice count, and a `suspected_spin_loop=1` hint for repeated `B .`.
6. Added `tools/verify_ncv_hybrid_dtb_apk.sh` to verify that the newly built APK contains the expected native engine ID, and `tools/symbolize_ncv_pc.sh` to map a PC to a symbol when the matching `vmlinux` is available.

## Validation actually performed here

- Native engine static library compiled successfully with host CMake.
- Focused Android boot/DTB and MTK probe test executable: **12/12 passed**.
- JNI bridge C++ syntax check passed against JDK 21 headers.
- A synthetic Android boot v2 image containing a DTB was parsed and transformed successfully. This is a parser/transformer test, not a real phone kernel boot test.
- Full large host test target did not finish compiling within the available environment timeout, so no claim is made that the entire suite passed.
- Android SDK/NDK/Gradle APK build and physical-device boot were not run in this environment.

## Important remaining limitation

This is not a full MT6768/MT6769 system emulator. It does not implement the real MTK clock/reset controllers, PMIC, IOMMU, storage controllers, Mali GPU, camera/audio/modem/Wi-Fi, vendor HAL stack, Android dynamic partition/device-mapper/AVB/fstab boot flow. Keeping vendor DT nodes in a DTB does not make the emulated hardware exist. The patch tests whether correct DTB adaptation and better diagnostics let the vendor kernel get further; it does not guarantee a bootable Infinix stock ROM or splash logo.

For broad device support, the scalable route is a common ARM64 CPU emulator plus a small set of virtual board profiles and an Android kernel/GSI built for the chosen virtual board. A physical Infinix stock kernel/userspace remains SoC/board-specific.
