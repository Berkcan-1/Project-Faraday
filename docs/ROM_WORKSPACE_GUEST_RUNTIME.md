# NCV ROM Workspace → Guest Runtime

## User flow

1. Choose `super.img` in ROM Workspace.
2. Wait for liblp metadata validation and the read-only logical-partition probe.
3. Choose the matching `boot.img` from the same firmware build.
4. Add `vendor_boot.img` and/or `init_boot.img` only when the image layout requires them.
5. Open Guest Runtime controls, review the boot profile, and explicitly start the kernel attempt.
6. Observe real NCV guest state, UART output, framebuffer writes, and diagnostic snapshots.

Changing `super.img` clears previously selected boot components so files from different firmware builds are not accidentally mixed. Document URIs are persisted when the provider allows it, and the Android boot files are validated as supported boot containers before they are accepted by the workspace.

## What the runtime path actually does

- The native boot-image parser reads supported Android boot containers and extracts the actual ARM64 kernel/initramfs payloads supported by the current implementation. When `boot.img` has no embedded DTB, NCV records the DTB payload from the selected matching `vendor_boot.img` as the next adaptation candidate; this does not guarantee it is the correct board DTB or that the adapted kernel will boot.
- The NCV software-emulated ARM64 guest is created, the boot image is loaded, and selected block-image file descriptors are attached through the current virtio block implementation.
- The actual guest CPU is started and UART/engine diagnostics are displayed.
- Runtime status now reports the strongest observed milestone separately: `KERNEL EXECUTING · STORAGE NOT PROBED`, `GUEST STORAGE I/O ACTIVE · ANDROID NOT REACHED`, `GUEST KERNEL CONSOLE ACTIVE · ANDROID UNCONFIRMED`, or `KERNEL WRITES FRAMEBUFFER · ANDROID UNCONFIRMED`. The generic “OS UNVERIFIED” label was replaced with the actual blocked stage.
- The default device-tree strategy is a clean NCV virtual-board DTB so the next bring-up attempt is not silently tied to the phone's original peripheral tree. An experimental hybrid phone-DTB option remains available for comparison.
- The VM now initializes the ARM64 Linux direct-entry state explicitly: `SCTLR_EL1` RES1 bits are set with MMU/data cache/instruction cache disabled, DAIF is masked, the DTB address is passed in `x0`, and `x1`–`x3` are zero.

## Current blockers / limitations

- For `super.img`, the current boot attempt wraps the image in a read-only synthetic GPT carrier with a partition named `super`. Additional host-side views expose active logical partition bytes through the liblp extents. This does not guarantee the guest kernel has virtio/GPT drivers or that Android first-stage init can resolve `/dev/block/by-name/super`; the guest's own `dm-linear` mappings, fstab/device discovery and complete AVB/verity path remain unverified.
- The `vendor_boot.img` DTB payload may contain multiple concatenated device trees on real devices; current NCV does not select one using a full board-compatible string match, so the first candidate can still be wrong. The current generic board is not a full MediaTek MT6768/MT6769 device model. The optional MTK probe profile only logs unknown MMIO accesses and uses zero/no-op fallbacks; it does not emulate clocks/resets, PMIC, watchdog, IOMMU, Mali, modem, audio, camera or proprietary vendor services.
- The current framebuffer is not Mali/HWC/SurfaceFlinger. Android `init`, ART, Binder system services, `system_server`, SurfaceFlinger and the OEM launcher have not been validated together.
- `vendor_boot.img` and `init_boot.img` are not universally required or universally optional: use components that match the firmware's actual boot header/ramdisk layout. A parser accepting them does not prove board compatibility.

This feature is a genuine kernel-entry experiment, not a mocked Android desktop and not a promise that a stock Infinix ROM will boot. Any claim of Android userspace success needs logs or other observable evidence from the real guest.


## Read-only liblp block views (diagnostic milestone)

- During guest preparation, NCV may attach each non-empty active logical partition as an additional read-only virtio-mmio block device. Its reads are translated through the selected liblp extents into the expanded bytes of `super.img`; Android sparse RAW/FILL/DONT_CARE chunks are handled by the existing backing-image reader. The implementation does not materialize multi-gigabyte partition copies.
- These are **NCV host-side linear views**, not Linux `dm-linear` targets inside the guest. They show up as additional virtio block devices if the guest kernel supports `virtio-mmio` and `virtio-blk`; NCV does not yet create `/dev/block/by-name/super`, patch ramdisk fstab, create guest `/dev/block/mapper/*`, or perform AVB/verity verification.
- Periodic logs report per-device MMIO reads/writes, block read/write requests, failed requests and capacities. If the counts remain zero, the guest kernel is not probing/using these exposed virtio devices; that is a kernel/device-tree compatibility blocker, not a partition-data problem.

## Synthetic GPT carrier for `super.img`

The Guest Runtime now has an optional `attachSuperContainerFd` path. It exposes the selected `super.img` as a read-only virtio disk with a synthesized protective MBR and primary/backup GPT headers. The single GPT entry is named `super`, starts at LBA 2048 (1 MiB alignment), and its data range maps directly onto the original expanded `super.img` byte stream. No copy or modification of the selected file is performed. Android sparse source images are expanded on reads using the existing sparse reader.

This is a virtual hardware compatibility aid, not a promise that the selected vendor kernel can discover the partition. The guest still needs a working `virtio_mmio`/`virtio_blk` driver and GPT partition support, after which Android first-stage init must discover `/dev/block/by-name/super`, read liblp metadata, and create guest dm-linear devices. NCV also exposes host-side logical partition views as extra virtio devices for diagnostics; those are not a substitute for guest device-mapper. Check `[NCV BLOCK IO]` counters to see whether the guest actually probes or reads the devices.

## Boot semantics and current next experiment

`super.img` is not itself a bootable kernel image. AOSP describes `super.img` as the container for logical system/vendor partitions and liblp metadata; the kernel and appropriate ramdisk live in `boot.img`, and depending on Android version/device design additional boot data can live in `vendor_boot.img` and/or `init_boot.img`. NCV therefore starts the real ARM64 kernel extracted from the selected matching `boot.img`, while exposing the selected `super.img` as read-only guest storage.

The latest supplied log shows the CPU executing kernel instructions, but after 20 seconds `mmio_reads=0`, `read_requests=0`, `fb_write_ops=0`, and no UART output. That is evidence that the guest has not probed NCV's virtual block devices or written to the virtual framebuffer; it is not evidence that Android userspace has started. A stock Infinix MediaTek kernel may not contain the `virtio-mmio`/`virtio-blk` drivers because physical Infinix devices do not use NCV's generic virtual board. The clean NCV DTB is now the recommended isolated experiment. If its counters remain at zero, the next step is to inspect/build a kernel with the needed virtio drivers or implement the relevant MediaTek device model; a UI status change alone cannot fix this.
