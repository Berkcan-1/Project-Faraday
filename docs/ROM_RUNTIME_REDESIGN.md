# NCV ROM Runtime Redesign

## Decision

The next milestone should focus on a **ROM userspace compatibility runtime and generic virtual-device boundary**, not expanding the custom ARM64 interpreter as the only route to a ROM UI.

This does not mean that framework code can be loaded directly from `super.img` and magically executed. An Android ROM's `system`, `system_ext`, `product`, `vendor`, and related logical partitions are filesystem images and packages. Running their real framework requires a compatible execution environment, Android runtime and system services, IPC, filesystem layout, security policy, and a display/input path.

## New first step in this branch

NCV now has a read-only `SuperImageMetadataParser` that can:

- Recognize the Android liblp geometry and metadata headers.
- Validate the geometry, metadata-header, and metadata-table SHA-256 checksums.
- Read raw `super.img` and Android sparse super images without expanding the complete image to RAM or disk.
- Enumerate logical partition names, sizes, extent counts, read-only flags, zero extents, and extents that refer to a separate block device.
- Fall back from a damaged primary metadata copy to the backup copy and try other metadata slots.
- Put hard limits on table sizes and counts before allocating memory.

The parser validates liblp metadata, and the current ROM workspace also opens real read-only logical-partition byte views by mapping the listed extents over the expanded raw/sparse image address space. It can probe a few bytes for an ext4/EROFS superblock signature. It still does not mount or extract ext4/EROFS, create `/dev/block/mapper/*`, apply AVB/verity, execute the ROM, or modify the selected file.

## Proposed architecture

```text
NCV Android UI
  ├── ROM import + format / compatibility report
  ├── Super image reader
  │     ├── raw image reader
  │     ├── Android sparse logical-address reader
  │     └── liblp metadata parser
  ├── Logical partition block view (next milestone)
  │     ├── system
  │     ├── system_ext
  │     ├── product
  │     ├── vendor
  │     └── odm / other supported partitions
  ├── Android userspace runtime (architecture decision required)
  │     ├── init + property service
  │     ├── Bionic + linker + ART
  │     ├── Binder + servicemanager + system_server
  │     ├── package manager / setup wizard
  │     └── SurfaceFlinger / WindowManager
  └── NCV generic virtual-device adapters
        ├── display + buffer transport
        ├── touch / keyboard
        ├── storage + userdata
        ├── clock / random / networking
        └── optional sensor, audio, camera adapters
```

## The execution model still needs a deliberate choice

### A. Same-architecture userspace on the host kernel

When the guest userspace ISA matches the host (for example, AArch64 ROM userspace on an AArch64 host), instruction-set emulation may be unnecessary. However, a compatibility runtime still needs the expected Linux system calls, filesystem layout, Binder interfaces, Android native libraries and services, process lifecycle, permissions, and display integration. A normal non-root Android application sandbox does not provide arbitrary mount namespaces, device-mapper setup, or permission to launch an unrelated system init as a new OS instance. Do not assume that copying the partition into an app-private directory is enough.

### B. A real virtual machine using hardware virtualization

A VM can run a guest kernel and userspace with virtual devices rather than attempting to reproduce each OEM SoC. This is the clearest system boundary, but it still needs a board definition, boot chain, CPU/ISA compatibility, guest drivers, storage mapping, graphics and input. On consumer Android devices, access to hardware virtualization facilities is platform/build dependent and commonly unavailable to an ordinary app; this must be tested on the intended host rather than assumed.

### C. Keep NCV's software CPU engine as an optional diagnostic backend

The current custom interpreter may remain useful for learning, synthetic guest tests and controlled bring-up. It should not be treated as the default route to support arbitrary EMUI/HyperOS images until a real Android kernel can boot reliably on the NCV board.

## Why a generic driver layer is valuable, but not sufficient alone

A generic adapter layer can provide stable virtual implementations for a display, touch input, storage, clock, random source and network interface. Above that boundary, Android's framework and services can remain closer to their normal behavior. However, vendor-specific HALs, daemons, system properties, SELinux policy, kernel interfaces, and proprietary services may still bind a ROM to the original device. A generic framebuffer is also not a complete Android graphics stack: the platform needs compatible buffer allocation, synchronization, composition and input plumbing before its real launcher or setup wizard can run.

## Milestones

1. **Super metadata inspection (implemented):** parse liblp tables and report logical partitions safely.
2. **Read-only logical partition reader (implemented):** translate logical reads to extents in the expanded raw/sparse `super.img` address space; return zeros for zero extents and reject external-device extents without their backing image. No writes to the ROM image.
3. **Filesystem inspection (next):** enumerate ext4/EROFS contents using a bounded read-only parser or a well-contained helper; locate framework libraries, manifests, boot-animation assets and setup-wizard packages. This is inspection, not execution.
4. **Execution-model proof:** choose host userspace compatibility or a VM backend and prove a tiny controlled guest/runtime works before claiming OEM ROM compatibility.
5. **Generic kernel/ramdisk boot path:** wire a kernel and initramfs supplied by NCV to the logical partition reader, instead of requiring an OEM `boot.img` as the target runtime's boot source. This step is not implemented yet.
6. **Android service and virtual-device integration:** establish kernel/userspace contracts, Binder and property services, storage/data persistence, display buffers and touch input.
7. **ROM profile compatibility:** compare `system` / `system_ext` / `product` with matching `vendor` / `odm`, boot image metadata, API level and ABI. Reject incompatible combinations with actionable diagnostics.
8. **Boot milestones:** distinguish `metadata parsed`, `partition bytes read`, `filesystem inspected`, `kernel booted`, `Android init reached`, `boot animation shown`, `setup wizard launched`, and `launcher reached`. Never treat one stage as evidence for a later one.

## Boot animation and setup wizard

A boot animation is an asset (`bootanimation.zip`) played by Android's boot-animation service once enough userspace services and the display path are ready. Extracting or playing the ZIP can be a useful early visual test, but it is not proof the Android framework is running.

The first-run setup wizard is normally an Android application that depends on the package manager, framework APIs, system services, Binder and graphics/input. It is not a standalone installation screen embedded in `super.img`. Some OEM customization packages can live in `product` or `system_ext`, and they may rely on matching vendor services. Therefore NCV should not promise that any arbitrary `super.img` will reach setup wizard or a launcher until that actual ROM runs in the selected execution environment.

## Current branch limits

- The metadata parser and host-side logical partition readers are read-only; no guest device-mapper nodes are created.
- The ROM workspace can probe bytes from actual logical partitions, but it does not mount ext4/EROFS filesystems.
- The existing diagnostic Android boot path still attaches `super.img` as one disk and uses the current software-emulated ARM64 engine.
- No generic Android userspace runtime or OEM-HAL compatibility layer is implemented by this change.
- No EMUI or HyperOS ROM has been booted or tested by this change.
- The Android Gradle/NDK build must be run in Android Studio; this working environment does not include the complete Android SDK/NDK toolchain.

## Technical references

- [AOSP: Implement dynamic partitions](https://source.android.com/docs/core/ota/dynamic_partitions/implement) — liblp metadata, logical partition extents, and device-mapper model.
- [AOSP: Partitions overview](https://source.android.com/docs/core/architecture/partitions) — system, system_ext, product, vendor, boot, and vendor-specific responsibilities.
- [AOSP: Android architecture overview](https://source.android.com/docs/core/architecture) — framework, system services, ART, native libraries, HAL and kernel boundaries.
- [AOSP source: liblp metadata format](https://android.googlesource.com/platform/system/core/+/master/fs_mgr/liblp/include/liblp/metadata_format.h) — serialized geometry, metadata headers, partitions, extents and block devices.
