# NCV MediaTek compatibility probe (experimental)

## What this profile does today

The UI exposes a profile named **MediaTek probe mode**. The native bus has an opt-in fallback for otherwise-unmapped guest physical accesses in the heuristic aperture `0x10000000` through `0x1fffffff`:

- Reads return zero.
- Writes are ignored.
- The last 24 unknown MMIO operations are traced with address, access size, operation and value; read/write totals and last addresses are included in the stop/fault report.
- Addresses outside the heuristic aperture remain unmapped faults.
- The default NCV profile keeps strict unmapped-access behavior.

NCV now has a set of **generic virtual-board** devices: PL011 UART, single-vCPU GICv2, ARM generic timer model, PL031 RTC, virtio-mmio block, virtio-rng with guest-RAM DMA, simple software framebuffer, and a limited single-vCPU PSCI HVC/SMC subset. These devices are described by NCV's generated DTB; they are not MediaTek hardware models.

The probe fallback is still diagnostic only. It is **not** an MT6768/MT6769/Helio G85 emulator and does not implement MediaTek register semantics. It does not emulate MTK clock/reset controllers, PMIC, watchdog, IOMMU, Mali GPU, camera, audio, Wi-Fi, modem, or proprietary Android vendor HAL services. The DTB describes NCV's virtual board, not an Infinix HOT 20 motherboard. The profile neither loads proprietary drivers from the host nor accesses host physical hardware. Simple framebuffer output is not GPU acceleration or an Android HWC/gralloc implementation.


## First real-boot stop: MDSCR_EL1 (fixed in source)

The reported instruction `0xD5100240` decodes to `MSR S2_0_C0_C2_2, X0`, which is the architectural `MDSCR_EL1` debug-control register (the raw sysreg encoding uses op0=2). It is not an MTK MMIO register and does not indicate a missing PMIC/GPU driver.

The interpreter now recognizes the common AArch64 debug sysreg bank used during early kernel setup. Because this virtual CPU does not provide hardware breakpoints, watchpoints, or single-step, those registers are modeled as RAZ/WI for now (reads-as-zero, writes-ignored); this is only sufficient for code that clears/configures debug state without depending on debugging features. The unsupported-instruction diagnostic now prints op0 as 2 or 3, rather than the raw encoding bit, and names `MDSCR_EL1` for this opcode.

This removes the exact stop in the submitted log, but does **not** prove the kernel will boot. Re-run the same `boot.img` + `super.img` attempt and inspect the next real stop. The next fault may be another system register, instruction, MMIO range, or a board/ramdisk mismatch; fix each using the next log rather than assuming the first fault was the only one.

## Booting only with `boot.img`

`boot.img`-only is an allowed **diagnostic attempt** when the image contains a valid kernel and a non-empty generic ramdisk. If its ramdisk size is zero, a separate generic ramdisk (`init_boot.img` in some Android 13+ launch-device layouts) or a custom monolithic boot image is required. Even when the parser accepts `boot.img`, vendor-specific DTB/ramdisk/modules and the kernel's board assumptions may still block boot. See the official [AOSP generic boot partition](https://source.android.com/docs/core/architecture/partitions/generic-boot) and [vendor boot partition](https://source.android.com/docs/core/architecture/partitions/vendor-boot-partitions) docs.

## Why an Infinix HOT 20 stock ROM is a different task

An OEM ROM is not just `system.img`. It contains a kernel/ramdisk configuration and vendor userspace tied to a specific hardware board. A generic GSI shares framework/system code but still depends on a compatible vendor interface. Reusing a phone's `boot.img` with NCV's virtual-board DTB is not expected to work. Reusing the phone's vendor DTB would require emulating the devices it describes, including its interrupt controller and clock/reset/DMA model. A zero-read/no-op shim cannot reproduce those semantics.

The practical order is:

1. Prove a known ARM64 Linux kernel boots on the NCV virtual board.
2. Add a versioned NCV Android board config and build a matching Android kernel + initramfs/fstab for that board. For a no-separate-vendor-boot image, package all required board/vendor ramdisk content into a monolithic boot bundle as supported by that AOSP product config.
3. Implement and test the exact virtual devices declared in the NCV DTB: interrupt controller, timer, UART, virtio block, later graphics/input/network.
4. Add Android partition mapping (`super` logical partitions, `vendor`, `userdata` overlay), verified-boot/verity policy for test builds, and the Android first-stage mount path.
5. Add guest-side HAL adapters built for the guest Android (power, sensors, graphics, audio, camera, Wi-Fi, etc.). These are guest components, not host-side fake strings. Return explicit “unsupported” for absent hardware rather than claiming a fake camera/network device works.
6. Only after this generic virtual-board GSI works, create an MTK compatibility target. Parse the target DTB and add known devices one at a time from documented register maps/traces. Each fake response must be per-register/state-machine, logged, and tested; generic zero/no-op fallback remains a last-resort diagnostic mode.

## Test result and limitations

Native Release, ASan/UBSan, and ThreadSanitizer host test executables each passed 107 tests in this edit pass, including a regression for `MSR MDSCR_EL1`. These are host unit/synthetic tests only. They do not prove a real kernel boot, Android boot, Infinix ROM support, or Android APK build.
