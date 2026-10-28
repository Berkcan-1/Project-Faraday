# NCV Super-backed Android Boot — Next Bring-up Stage

## What changed

- Guest Runtime defaults to a clean NCV virtual-board DTB, with the Infinix/MediaTek hybrid DTB retained as an explicit experimental option.
- Runtime status no longer uses the vague `OS UNVERIFIED` label. It reports observed UART, virtio block-I/O, framebuffer, or no-device-probe milestones.
- Linux ARM64 direct entry explicitly initializes `SCTLR_EL1` to `0x30D00800` (architectural RES1 bits, `M/C/I` clear), masks DAIF, passes the DTB physical address in `x0`, and zeros `x1`-`x3`.
- The native test for ARM64 Linux direct boot now asserts these entry-register conditions.
- When `super.img` is chosen, the start-button label describes the actual pair: `super.img` provides dynamic-partition storage, while the matching `boot.img` provides the kernel/ramdisk. `super.img` alone is not a kernel boot image.

## Why the last run is black

The supplied log reports a 2 GiB guest RAM capacity and continued CPU instruction retirement. But after the 20-second gate, all six virtio devices still had `mmio_reads=0`, `read_requests=0`; `fb_write_ops=0`; no UART output was observed. Therefore the guest has not demonstrated storage-device discovery or framebuffer output. Android `init`, ART, Binder, SurfaceFlinger and boot animation have not been demonstrated.

## What this does *not* solve yet

- The generic DTB describes virtio-mmio devices, but the selected stock Infinix kernel must include a compatible `virtio_mmio`/`virtio_blk` driver and actually probe those addresses. The current log suggests it does not.
- The phone-specific MediaTek MT6768/MT6769 clock/reset, PMIC, UART, storage-controller, IOMMU and GPU models remain incomplete. The MTK probe shim is only a zero/no-op diagnostic fallback, not a SoC model.
- NCV's host-side logical partition views are not Android's guest `dm-linear` targets. First-stage init must still locate `super`, read liblp metadata, create mapper devices and mount the correct filesystems/verity chain.
- The NCV simple framebuffer is not the phone's Mali/HWC/SurfaceFlinger path.
- The Android APK was not built in this environment; run the project from Android Studio and test against the same firmware set.

## Suggested interpretation of the next log

- `mmio_reads > 0`, `read_requests > 0`: virtual storage probing/reading began; inspect failed requests and first-stage mount results next.
- `mmio_reads > 0`, `read_requests = 0`: the driver is probing but block request processing has not completed.
- All block `mmio_reads = 0`: kernel/DTB compatibility or missing guest drivers; do not keep adding host-side partitions as if they were guest `dm-linear` devices.
- `fb_write_ops > 0`: a real guest framebuffer write occurred, but that does not prove Android graphics or boot animation.
- UART output: real console bytes were emitted; interpret their content as the next boot milestone.

## References

- Linux ARM64 boot protocol: `x0` holds the DTB physical address, `x1`-`x3` are zero, interrupts are masked, and MMU is off at entry.
- AOSP generic boot partition docs: `boot.img` carries the kernel; the generic ramdisk is in `boot.img` on Android 12 and older, while devices launching with Android 13 move it to `init_boot.img`.
- AOSP dynamic partition docs: `super.img` contains logical partition images and liblp metadata, not a replacement for the separate kernel boot image.
