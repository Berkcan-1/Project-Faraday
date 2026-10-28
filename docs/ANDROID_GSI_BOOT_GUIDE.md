# NCV Android GSI boot profile guide

Updated: 2026-10-26

## Current state in this source package

NCV now has a boot-profile screen and a native path that attempts to parse an Android kernel boot bundle, place the kernel/initramfs in guest RAM, attach selected system/vendor images as read-only virtio-mmio block devices, generate an NCV DTB, and start the software-emulated CPU.

That is an experimental **kernel-entry attempt**, not yet a successful Android boot. A running vCPU thread or a selected `system.img` must never be reported as “Android booted.”

## Booting with only boot.img

The UI/native path now permits a diagnostic attempt with `boot.img` + `system.img` (or `super.img`) even when no separate `vendor_boot.img` is selected. This works at the parser/loader level only if `boot.img` contains a kernel and a non-empty generic ramdisk. Legacy header versions commonly carry both in `boot.img`; some Android 12-era GKI builds can also carry a ramdisk there. For devices launched with Android 13, the generic ramdisk is commonly split into `init_boot.img`, so a kernel-only `boot.img` cannot boot Android by itself. Check your actual header and ramdisk size with AOSP's `unpack_bootimg.py`; do not infer the layout from the Android version alone. Official references: [Generic boot partition](https://source.android.com/docs/core/architecture/partitions/generic-boot), [boot image header](https://source.android.com/docs/core/architecture/bootloader/boot-image-header).

`vendor_boot.img` is parser-optional for the monolithic boot-only experiment, but real GKI device configurations commonly need its vendor ramdisk, DTB, bootconfig and modules. The current NCV path generates an NCV virtual-board DTB rather than using an arbitrary phone DTB. Therefore this is useful for a kernel/ramdisk built for NCV, not a way to run an arbitrary Infinix stock kernel with only `boot.img`.

## Files the UI can accept

- `system.img`: GSI/system partition. It is attached as a read-only block device; this alone does not make Android mount it.
- `boot.img`: Android boot container with a supported kernel payload.
- `vendor_boot.img`: matching vendor ramdisk/DTB/bootconfig container for boot header v3/v4 profiles. It should come from the same virtual-board build as `boot.img`.
- `init_boot.img`: optional v4 ramdisk-only component used by Android 13+ launch-device layouts when the generic ramdisk was moved out of `boot.img`.
- `vendor.img`: optional read-only partition attachment. It is not a substitute for the correct vendor ramdisk, fstab, board configuration, or a dynamic-partition map.

The native parser performs structural range validation. It does not authenticate images with AVB, and it does not prove two files belong to the same Android build.

## Required matching profile

All boot components must match one another and the virtual board. Do not combine a GSI with random `boot.img` / `vendor_boot.img` / `vendor.img` files extracted from a physical phone. A phone kernel/vendor setup normally expects that phone's MediaTek/Qualcomm/etc. hardware, hardware services, clocks, buses and partitions. NCV's current board instead exposes a simplified ARM64 board with PL011 UART, GICv2 and virtio-mmio block devices.

For a reproducible Android boot target, NCV needs a dedicated Android virtual-board build with at least:

1. A kernel configured for the NCV virtual board and the implemented interrupt controller, timer, UART and virtio devices.
2. A matching vendor ramdisk and fstab that know how the selected system/vendor partitions are exposed.
3. A compatible vendor userspace/HAL set. A generic system GSI does not provide vendor HALs by itself.
4. A complete partition layout. Android dynamic partitions (`super`/logical partitions), AVB/verity and writable userdata are not implemented as a complete boot flow in this version.
5. Android-compatible virtual graphics and input, including the guest-side display/HWC path needed by SurfaceFlinger. This version does not provide that path.

Because the matching NCV Android virtual-board build does not exist yet, there is no arbitrary GSI + phone boot-image combination that can be expected to reach Android home in this version. A boot.img-only attempt is not the same as compatibility with an OEM ROM.

## Super-backed diagnostic route (new)

Before trying the OEM MediaTek kernel again, use the **Boot NCV kernel + super.img storage** button shown when `super.img` is selected. It boots the packaged NCV generic Linux kernel and diagnostic BusyBox initramfs, attaches `super.img` as a read-only GPT carrier, and exposes extra read-only logical-partition extent views. The initramfs enumerates `/proc/partitions` and attempts one-sector reads from each visible `/dev/vd*` device, reporting `BLOCK_READ_OK` over UART. This is a genuine guest-kernel/storage test; it does not run Android `init`, ART, Binder services, SurfaceFlinger, or the OEM boot animation.

To include the diagnostic kernel and initramfs in the APK, ensure `guest-board/ncv_linux_bringup.config` exists and set `ncv.linuxKernelSource` and `ncv.busyboxSource` in `local.properties`. The pre-build Gradle task builds an ARM64 kernel with built-in virtio-mmio/virtio-blk and a static BusyBox initramfs, then packages them as assets. If the button reports that these assets are missing, the APK was built without configured source trees or without the artifact task running.

## Recommended development path

1. **Prove Linux first.** Build a small ARM64 kernel/initramfs specifically for NCV's current DTB. Enable PL011 console and virtio-mmio/block in that kernel. Load a raw/gzip ARM64 `Image`, generate the NCV DTB, and require meaningful kernel console output before treating the guest path as healthy.
2. **Finish the Android CPU/architecture requirements.** Fix unsupported ARM64 instructions, system registers, exception paths and timer/interrupt behavior based on real kernel logs.
3. **Freeze a virtual-board ABI.** Document the RAM map, interrupt map, UART, virtio-mmio addresses, boot arguments and DTB compatibility. Do not change it without a board-profile version.
4. **Build a matching Android guest.** Produce kernel, `boot.img`, `vendor_boot.img`, optional `init_boot.img`, ramdisk/fstab and vendor images for the NCV board, then integrate a compatible GSI.
5. **Implement Android storage semantics.** Add the logical/by-name partition mapping and dynamic partitions, AVB/verity policy and writable userdata lifecycle expected by the guest build.
6. **Implement graphics and input.** Only show an actual Android desktop when guest pixels/frames are coming from the VM and an Android graphics stack has initialized.
7. **Test a small compatibility matrix.** First one known Android build, then Android 12–17 one by one. Future versions such as Android 18–25 are compatibility goals only, not promises.

## What the UI does

1. Import `system.img`.
2. Select a compatible `boot.img`.
3. Select its matching `vendor_boot.img` where required.
4. Select the matching `init_boot.img` if that build uses it for the generic ramdisk.
5. Optionally select `vendor.img`.
6. Tap **Attempt Android boot**. The UI shows parser/loader errors or UART output and uses “unverified” status labels until actual OS boot is proven.

The selected system/vendor image descriptors are attached read-only. Image selection does not rewrite the imported files. The VM's 512 MiB experimental Android profile is currently a bring-up setting, not a realistic memory guarantee for a full Android userspace.

## Verification for the current package

- Host C++ engine target: compiled successfully in the latest pass.
- Full native test executable rebuild: did not finish within the current run-time window; no fresh full-suite pass is claimed here. Previous stage reports are historical snapshots and cover synthetic/unit tests, not OS boot.
- Shell syntax checks for the kernel-build and test-initramfs scripts: passed.
- Initramfs packer smoke test: created and listed a cpio archive with a host test binary; the packer now rejects non-AArch64 binaries, so that host artifact is not a usable guest image.
- Android Gradle/NDK build: not performed in the current development environment.
- Install/device test: not performed.
- Real Linux kernel boot: not verified.
- Real Android/GSI userspace boot and graphical display: not verified.

## Local bring-up and owner tasks

For MTK probe mode boundaries and the path to a real device-specific compatibility layer, see [`MTK_COMPATIBILITY_PROBE.md`](MTK_COMPATIBILITY_PROBE.md). For a detailed Turkish explanation of `boot.img` vs `vendor_boot.img` vs `init_boot.img`, local Gradle/NDK build steps, building a Linux kernel plus BusyBox test initramfs, and the developer-owned tasks required for a matching NCV Android guest, see [`OWNER_SETUP_AND_BOOT_GUIDE_TR.md`](OWNER_SETUP_AND_BOOT_GUIDE_TR.md).
