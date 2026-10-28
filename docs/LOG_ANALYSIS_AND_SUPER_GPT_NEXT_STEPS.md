# NCV guest boot log analysis and next steps

Date: 2026-10-28

## What the supplied log establishes

- The Android boot image v2 was parsed; an embedded DTB and a 12.7 MiB initramfs were loaded.
- The guest CPU entered EL1 and the program counter progressed over many runtime snapshots. The log showed no data abort in the recorded snapshots. This means the guest kernel was executing; it does not prove that Android `init` started.
- The source image's liblp metadata parsed, with active `product_a`, `system_a`, `system_ext_a`, and `vendor_a` entries. The `_b` entries were empty/disabled in that metadata.
- The previous implementation exposed the complete `super.img` as one unpartitioned virtio disk. It did not expose a GPT partition labelled `super`, nor did it create guest `/dev/block/mapper` devices.
- The displayed virtual board is not an MT6768/MT6769 implementation. It uses a generic NCV GIC/timer, virtio devices, PL011 UART and a simple framebuffer. It does not model the MediaTek storage/controller stack or Mali/HWC graphics.

## Implemented in this change

- Added a read-only synthetic GPT carrier for `super.img`: protective MBR, primary and backup GPT headers/tables, a partition entry named `super`, and a 1 MiB-aligned partition range that maps reads directly to the selected source image. The source file is not modified or copied.
- Existing Android sparse-image reads are supported behind this carrier.
- Added host-side read-only views for valid liblp extents as extra virtio disks. They allow inspection and block-level diagnostics, but are explicitly not guest Linux `dm-linear` targets.
- Added per-device `mmio_reads`, `mmio_writes`, `read_requests`, `write_requests`, and `failed_requests` counters to the guest log.
- Added a host test that reads the synthetic MBR, validates the primary GPT header CRC, checks the `PARTNAME=super` entry, and confirms that reads through the virtual partition map to the source image.

## How to interpret the next device diagnostics

- `mmio_reads=0` and `mmio_writes=0`: the guest kernel has not accessed that virtio-MMIO device. The selected kernel may lack the required virtio drivers, may not have reached device probing, or the adapted DTB/device addresses may be incompatible.
- MMIO counters increase but `read_requests=0`: the driver probed/configured the device but has not submitted block reads yet.
- `read_requests>0` and `failed_requests=0`: actual guest block reads are completing against NCV's backing image; this still does not prove first-stage Android mount succeeded.
- `failed_requests>0`: inspect queue setup, request bounds, and read status before proceeding.

## Remaining work before Android can boot

1. Verify the target kernel has working `virtio_mmio` and `virtio_blk` support and that it consumes the adapted DTB. A stock phone kernel is not guaranteed to include those drivers.
2. Make first-stage init's fstab and device discovery match this virtual board so `/dev/block/by-name/super` resolves to the synthetic partition. The correct `androidboot.boot_devices` depends on the guest's device discovery and must not be guessed.
3. Let Android's real first-stage init read liblp and create guest dm-linear devices using the kernel device-mapper implementation. NCV does not synthesize a fake “Android booted” state.
4. Meet the selected ROM's verified-boot/AVB and userdata requirements.
5. Implement a graphics path compatible with Android's gralloc/HWC/SurfaceFlinger stack. A simple framebuffer alone is not the same thing as the original MediaTek Mali display stack, so no OEM boot animation is expected yet.
6. Guest RAM can be selected as 512 MiB, 1 GiB or 2 GiB. The 2 GiB guest capacity uses lazy 4 KiB pages, so NCV does not eagerly allocate 2 GiB on the host. The resident host allocation still grows when the guest writes non-zero data; RAM capacity alone does not fix unprobed storage or missing device support.

AOSP describes dynamic partitions as guest first-stage userspace parsing super metadata and creating dm-linear block devices; the existence of host-side extent views alone does not implement that boot flow: https://source.android.com/docs/core/ota/dynamic_partitions/implement

## Validation

- Native host suite: 135 tests passed, including lazy 2 GiB RAM capacity, cross-page access and framebuffer counters.
- JNI C++ source syntax check: passed using the host JDK JNI headers.
- Full Android APK build: not run in this environment because the Gradle Wrapper JAR and Android SDK are absent.
- Real device ROM boot: not yet verified.
