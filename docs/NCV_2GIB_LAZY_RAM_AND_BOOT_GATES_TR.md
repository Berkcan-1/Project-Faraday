# NCV — 2 GiB Lazy RAM and Android Boot Gates

## Implemented in this iteration

- Guest RAM supports capacities up to 2048 MiB. RAM is backed with lazily allocated 4 KiB pages; unwritten pages read as zero and zero stores to absent pages do not allocate memory. This is guest-visible capacity, not a promise that the host can sustain a fully resident 2 GiB working set.
- Guest Runtime exposes 512 MiB, 1 GiB, and 2 GiB options, with 2 GiB selected by default for the experiment.
- Runtime snapshots report guest RAM capacity and currently resident guest RAM page bytes.
- The simple framebuffer reports reads/writes/non-zero writes. Runtime emits one-time storage, Android-init, and graphics gate warnings if no device activity is observed after 20 seconds. These are diagnostic signals, not emulated Android services.
- Existing read-only `super` carrier and liblp extent-backed host-side block views remain attached.

## What the supplied Hot 20 logs demonstrated

The prior session showed six attached virtio block devices, but `mmio_reads=0`, `read_requests=0`, and no framebuffer device model. Therefore the guest kernel had not discovered/probed virtio storage, and no guest block I/O or framebuffer output was demonstrated. Merely adding more host-side logical-partition views cannot fix missing guest drivers or a DTB/boot protocol mismatch.

## Still not implemented / not claimed

- No guest-side Android `dm-linear` target is synthesized by the host. Android `init` must find the `super` device, read liblp metadata, and use the guest kernel's device-mapper support to create its own logical block devices.
- NCV is not an MT6768/MT6769 SoC model. MediaTek clock/reset controllers, PMIC/watchdog, IOMMU, Mali GPU, HWC, modem, Wi-Fi, camera/audio, and vendor HAL behavior are not modeled.
- A simple framebuffer is not Android SurfaceFlinger/HWC or a Mali GPU; no stock boot animation is implied.
- No full Android userspace boot has been verified. The Android SDK/APK build is not part of the host native test result.

## Validation

Host CMake suite: 135 native tests. Additional tests cover 2 GiB guest capacity with zero resident pages, last-byte access, cross-page writes, zero-write behavior, load/read across sparse pages, and framebuffer activity counters.
