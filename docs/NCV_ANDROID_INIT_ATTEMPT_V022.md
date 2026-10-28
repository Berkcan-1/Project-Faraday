# NCV 0.1.22 — Android first-stage init attempt

## Goal

The prior Infinix-kernel runs did not reach kernel UART output or virtio probing. The engine marker in the supplied runtime log was `0.1.18-dev-ncv-gzip-asset-name-fix-20261028`, which proves that the installed APK's native library was stale relative to newer source. Separately, the stock Infinix kernel expects MediaTek hardware that the NCV generic virtual board does not implement. Changing only its DTB cannot supply missing SoC devices or virtio drivers.

This version adds a separate, honest attempt to get the **real Android ramdisk** to run on NCV's own VirtIO-capable ARM64 Linux kernel:

1. Load the packaged `ncv/guest/diagnostic/Image` built for the NCV virtual board.
2. Parse the selected `boot.img`, plus matching optional `vendor_boot.img` and `init_boot.img`, without loading the phone's OEM kernel.
3. Extract the real ramdisk fragment(s) and vendor bootconfig, preserving the Linux initramfs format and bootconfig trailer.
4. Load the initramfs into guest RAM and pass a clean NCV board DTB containing the UART and VirtIO-mmio devices.
5. Attach the user's `super.img` read-only through a GPT carrier labelled `super`; separately attach read-only logical-partition extent views for diagnostics.
6. Keep the OEM-kernel experiment as a secondary button and block boot actions when the loaded native engine marker is stale.

## Build/install

Run on the Linux machine with the Android SDK/NDK, Gradle 8.13, AArch64 cross-compiler, and kernel/BusyBox source trees configured in `local.properties`:

```bash
./tools/build_verify_ncv_super_bringup.sh --install
```

The script deliberately removes Android Gradle Plugin native intermediates before `assembleDebug` so CMake cannot silently preserve the stale `libncv.so` that showed up in the previous logs. The resulting app must report this exact marker:

```text
0.1.22-dev-kconfig-compatibility-20261028
```

If it reports `0.1.18-dev-...`, do not repeat another boot attempt; the old native library is still installed or the wrong APK was installed. The project cannot build the APK in an environment without the configured Android SDK/Gradle and kernel/BusyBox sources.

## Run

1. Open Guest Runtime from ROM Workspace.
2. Select the `super.img` and the matching firmware's `boot.img`.
3. Select `vendor_boot.img` only if the same build needs it (typically Android boot header v3/v4); select `init_boot.img` only if the generic ramdisk is separate (common for newer Android launch devices). Do not mix components from different firmware builds.
4. Ensure private userdata is created if the image's first-stage mount needs writable `/data`.
5. Tap **Try Android userspace · NCV kernel + original ramdisk**. The secondary **Stock Infinix kernel experiment** continues to test the OEM kernel and is expected to encounter missing MTK hardware support.

## What to look for

The first useful milestones are real UART messages from NCV's generic kernel, VirtIO-mmio reads, successful block read requests, and eventually an Android `/init` or first-stage mount/device-mapper message. The new path's status label says “ANDROID INIT ATTEMPT RUNNING” by design; that label is an attempt status, not a success claim. If no UART output or virtio reads appear, the run has not demonstrated Android init.

For the older log pasted by the user, the strongest clues were:

- native engine still `0.1.18-dev-ncv-gzip-asset-name-fix-20261028`;
- after 20 seconds, zero virtio-MMIO reads and zero block reads;
- no guest UART output or framebuffer writes;
- repeated CPU progress only, with no proof of `init` or Android userspace.

## Known blockers

This is not a full ARM phone emulator. The NCV kernel must itself execute correctly in the interpreted CPU, its VirtIO drivers must match NCV's DTB, and the Android ramdisk must be compatible with the selected system/vendor build. The current virtual `super` carrier is not guaranteed to yield the OEM fstab's expected by-name and `boot_devices` paths. The NCV host-side logical-partition views do not create guest `/dev/block/mapper/*` nodes; Android first-stage init still must discover the disk and configure device-mapper linear targets. AVB/verity, SELinux policy, required firmware/vendor partitions, and a writable userdata filesystem may block progress.

Even if first-stage init and dynamic partitions work, the full Infinix userspace/boot animation is not expected yet. NCV does not emulate the target MediaTek clock/reset controllers, PMIC, IOMMU, Mali GPU/HWC, camera/audio/modem/Wi-Fi, and the proprietary vendor HAL environment. A real Android boot animation requires much more than showing the original ramdisk or starting a vCPU.

## Validation

Validated in the authoring environment:

- 138 native C++ unit/synthetic tests pass.
- JNI C++ translation unit passes a host `-fsyntax-only` check.
- Static asset/version/UI gates pass.
- Build and APK verification scripts pass `bash -n`.

Not validated in the authoring environment:

- Android Gradle/NDK APK build or installation;
- target-device boot attempt;
- successful Android `/init`, dynamic partition mount, Binder/ART/services, SurfaceFlinger, boot animation, or launcher.


## Linux Kconfig compatibility fix (v0.1.22)

The kernel builder no longer assumes that all Linux versions expose the same Kconfig symbols. It generates an
output fragment per kernel source tree:

- If `CONFIG_DM_LINEAR` exists, it enables it. If not, it verifies that `dm-linear.o` is linked into `dm-mod`
  (the arrangement used by current mainline kernels).
- It selects `CONFIG_ANDROID_BINDER_IPC_RUST` plus `CONFIG_RUST` on trees that provide Rust Binder; otherwise,
  it selects legacy `CONFIG_ANDROID_BINDER_IPC` and BinderFS only when those symbols exist.
- It enables SELinux's required `AUDIT`, `NET`, `INET`, and `SECURITY_NETWORK` dependencies.
- If the expected Binder implementation cannot be enabled, it reports the final relevant `.config` lines and
  gives a Rust toolchain hint rather than silently pretending Android prerequisites are present.

The script uses `tools/prepare_ncv_kernel_config_fragment.sh` to prepare this compatibility fragment.
`tools/test_ncv_kernel_config_compat.py` checks both current-mainline-style and legacy Kconfig layouts.
These are config-selection tests; they are not a real kernel build or proof of Android boot.
