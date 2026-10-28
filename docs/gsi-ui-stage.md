# NCV GSI import + guest screen — implementation status

Updated: 2026-10-26

## Included in this package

- A dependency-light Android app scaffold (Kotlin Activity + platform Views; no Compose/AppCompat dependency).
- A dark NCV landing screen, selected-image metadata, boot-profile controls, and a serial guest console.
- Storage Access Framework import. The app asks only for the exact URI selected by the user; it does not request broad storage permissions.
- Lightweight detection for ARM64 Linux `Image` / `Image.gz`, Android boot containers, GSI/system partitions, Android sparse images and raw ext4 images.
- Native Android boot image parser for `boot.img` header versions 0-4 and `vendor_boot.img` versions 3-4. The parser does structural range checking only; it does not verify AVB signatures.
- Android 13+ `init_boot.img` generic-ramdisk extraction path for header-v4 ramdisk-only images.
- Read-only guest block backend for raw and Android sparse images, attached through a modern virtio-mmio block device.
- Read-only liblp metadata parser for raw and Android sparse `super.img`: validates geometry/header/table SHA-256 checksums, enumerates logical partition names and sizes, and reports extent flags. This is metadata inspection, not logical partition mapping. The VM exposes the device nodes in its generated NCV Linux DTB.
- Experimental boot-profile UI that allows selecting `system.img`, `boot.img`, `vendor_boot.img`, optional `init_boot.img`, and optional `vendor.img`. The UI attaches selected partitions read-only, prepares an initramfs and NCV DTB, then starts the software-emulated CPU. The optional MediaTek probe profile enables a bounded, logged zero-read/ignored-write fallback for unknown physical accesses; it does not emulate a real MTK SoC or vendor HAL.
- A raw/gzip ARM64 Linux Image direct-boot path, currently intended for bring-up/debugging through PL011 UART. The Linux kernel screen now allows an optional `initramfs.cpio.gz` selection and an explicit boot-attempt button; it no longer auto-starts before the developer can attach a test initramfs.
- Android Gradle configuration targeting API 36 (Google Play's new-app target requirement on/after 2026-08-31) and minSdk 31.

## What this does not prove

Selecting a boot profile now reaches a real *kernel boot attempt* rather than stopping because the parser or block device is missing. It does not prove that any real Linux kernel boots, and it does not mean an Android GSI boots to userspace or a graphical home screen.

The key unresolved blockers are:

1. A complete-enough ARM64 CPU/system-register/exception implementation for the selected real kernel; the current CPU model is still partial.
2. Exact boot profile compatibility. Android 12 launch/upgrade layouts differ, and Android 13+ launch devices commonly put the generic ramdisk in `init_boot.img`. `boot.img`, `vendor_boot.img`, `init_boot.img`, system and vendor images must come from a mutually compatible build.
3. The NCV virtual board currently uses its own generated DTB rather than a matching Android emulator DTB. The vendor DTB is parsed but is not used as if it matched the NCV device map.
4. Android's dynamic partitions are only partially inspected: NCV can parse `super.img` liblp metadata, but does not yet expose each logical partition as a separate `/dev/block/mapper/*` device. Device-mapper, AVB/verity and writable `userdata` lifecycle are not implemented as a complete Android boot flow. The optional vendor partition attachment is not a replacement for a full by-name partition/metadata setup.
5. GICv2, timer, PSCI and other device/system behavior are partial, not a validated Android emulator board.
6. The guest screen can display pixels written to NCV's simple 640x480 software framebuffer, but no virtio-gpu/Mali/GPU acceleration, Android HWC/gralloc, input device, or complete Android window/display integration is implemented. The guest screen intentionally does not draw a fake Android desktop.
7. No Android NDK build, install on a phone, real Linux boot, or Android/GSI boot has been verified in this environment.

## How to make a boot attempt

1. Select a `system.img` / GSI.
2. In the boot profile, select a compatible `boot.img`.
3. Select matching `vendor_boot.img` when the build requires vendor ramdisk/DTB content. `boot.img` alone is accepted only if it embeds a non-empty generic ramdisk; some Android 13+ launch layouts instead place it in `init_boot.img`.
4. For Android 13+ launch-device layouts where the generic ramdisk is separate, select matching `init_boot.img`.
5. Optionally add `vendor.img`. The selected block images are attached read-only.
6. Tap **Attempt Android boot** and inspect the actual UART text or fault state. A running emulation thread is not proof of a successful OS boot.

Do not mix a GSI with random `boot`/`vendor_boot` files from an unrelated physical phone. A normal phone's kernel/vendor configuration usually expects its own physical hardware, which is different from the NCV virtual board.

## Build verification status

Host C++ build and native tests were run in this workspace. The Android SDK/NDK/Gradle toolchain was not available here, so APK compilation and a physical-device test have not been run. The project uses Android Gradle Plugin 8.13.2 and should be built with a compatible Gradle 8.13 wrapper/distribution, not Gradle 9.6.

## Next steps for genuine GSI boot

1. Build a reproducible Linux bring-up image specifically for the NCV board and prove that the actual kernel emits boot logs and reaches userspace.
2. Complete the guest ARM64 instruction/system-register behavior demanded by that kernel and fix issues from real logs.
3. Define a reproducible Android virtual-board build: kernel configuration, DTB, virtio device map, vendor ramdisk, fstab, and matching vendor/system artifacts.
4. Implement the required Android partition model (including dynamic partitions/super and userdata handling) and validate init/first-stage mount.
5. Implement display and input and only mark “Android booted” after a verifiable launcher/framebuffer milestone.


For user-side instructions and explanations of `boot.img`, `vendor_boot.img`, `init_boot.img`, `system.img`, `vendor.img`, `super.img`, and `userdata.img`, see [`OWNER_SETUP_AND_BOOT_GUIDE_TR.md`](OWNER_SETUP_AND_BOOT_GUIDE_TR.md).


## Super image metadata inspection

Selecting a `super.img` now schedules a background metadata scan. The scan can read raw and Android sparse images, checks liblp geometry and metadata checksums, then prints the logical partition table and extent information to the session log. It does not expand the full image into memory or write to the source. The current boot attempt still attaches the image as one block disk; the logical partitions are not yet mounted or independently addressable by the guest. The broader execution-model proposal is in [`ROM_RUNTIME_REDESIGN.md`](ROM_RUNTIME_REDESIGN.md).
