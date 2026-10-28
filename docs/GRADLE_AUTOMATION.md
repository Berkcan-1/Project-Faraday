# NCV Android Studio build automation

This project uses Android Gradle Plugin + `externalNativeBuild`/CMake. A normal Android Studio `assembleDebug` or `assembleRelease` already compiles the C++ emulator into `libncv.so` for `arm64-v8a` and packages it in the APK. The Gradle tasks in `app/build.gradle.kts` add an optional, repeatable bring-up pipeline for a *diagnostic Linux guest*.

## What the build does

Every Android build runs these project tasks before the app is packaged:

- `verifyNcvNativeSources`: checks that the C++ sources listed by Android CMake exist. Android's normal CMake/NDK build then compiles `libncv.so`.
- `prepareNcvApkAssets`: packages the NCV board config, a guest-artifact manifest, and any optional artifacts that have been built below.
- `buildNcvDiagnosticKernel`: only runs when `ncv.linuxKernelSource` is configured and guest artifacts are enabled. It uses `tools/build_ncv_kernel.sh` to build a raw ARM64 Linux `Image`, **not** Android `boot.img`.
- `buildNcvStaticBusyBox` and `buildNcvTestInitramfs`: only run when `ncv.busyboxSource` is configured and guest artifacts are enabled. They build a static ARM64 BusyBox and package a diagnostic `initramfs.cpio.gz` with `/init`.

If no diagnostic source paths are configured, the project still builds the C++ engine and APK. The build creates the manifest/board assets but does not try to download code or run long kernel builds unexpectedly.

## Configure it on your machine

Install the Linux cross toolchain and host packaging tools once:

```bash
sudo apt update
sudo apt install build-essential bc bison flex libssl-dev libelf-dev \
  gcc-aarch64-linux-gnu binutils cpio gzip git python3 rsync
```

Make sure you have a *standard Linux kernel source tree* that supports `make ARCH=arm64 ... Image` and a BusyBox source tree. These are diagnostic guest sources; they are not the Android kernel build from AOSP, and they are not `boot.img`/`vendor_boot.img`.

Add the following lines to the existing project `local.properties` file (do not erase its `sdk.dir` line). Use absolute paths:

```properties
ncv.linuxKernelSource=/home/YOUR_USER/src/linux
ncv.busyboxSource=/home/YOUR_USER/src/busybox
ncv.crossCompile=aarch64-linux-gnu-
ncv.buildJobs=6
ncv.packageGuestArtifacts=true
ncv.forceGuestRebuild=false
```

You can also set these values as Gradle project properties or environment variables:

| `local.properties` key | Gradle property | Environment variable | Purpose |
|---|---|---|---|
| `ncv.linuxKernelSource` | `ncvLinuxKernelSource` | `NCV_LINUX_KERNEL_SOURCE` | Standard Linux source tree used for diagnostic ARM64 `Image` |
| `ncv.busyboxSource` | `ncvBusyBoxSource` | `NCV_BUSYBOX_SOURCE` | BusyBox source tree used to build a static AArch64 binary and test initramfs |
| `ncv.crossCompile` | `ncvCrossCompile` | `NCV_CROSS_COMPILE` | Cross toolchain prefix; default `aarch64-linux-gnu-` |
| `ncv.buildJobs` | `ncvBuildJobs` | `NCV_BUILD_JOBS` | Parallel build job count; defaults to at most 8 |
| `ncv.packageGuestArtifacts` | `ncvPackageGuestArtifacts` | `NCV_PACKAGE_GUEST_ARTIFACTS` | Enables optional diagnostic artifacts and their APK packaging; default true when source paths are configured |
| `ncv.forceGuestRebuild` | `ncvForceGuestRebuild` | `NCV_FORCE_GUEST_REBUILD` | Forces configured kernel/BusyBox tasks to rebuild on every build while true |

The source trees are not downloaded automatically. This keeps Android Studio builds reproducible and avoids secretly fetching/building third-party code. If your AOSP kernel is Bazel/Kleaf-based, do not assume it is a compatible `make` kernel tree; use a supported standard Linux kernel tree for this diagnostic step.

## What goes into the APK

When configured and built, assets are copied into Gradle's generated assets directory and packaged in the APK:

```text
ncv/guest/manifest.json
ncv/guest-board/ncv_linux_bringup.config
ncv/guest-board/README.md
ncv/guest/diagnostic/Image                  # only if diagnostic kernel build is enabled
ncv/guest/diagnostic/initramfs.cpio.gzip       # only if BusyBox/initramfs build is enabled
```

If the diagnostic kernel is packaged, the app shows a **Bundled Linux diagnostic** card. It starts the included kernel and optional test initramfs through the same experimental UART path. That is strictly a Linux bring-up test, not Android or GSI.

Build outputs are stored under `app/build/ncv-guest/`. Gradle reuses the existing artifacts when possible. After changing the guest kernel/BusyBox source and wanting a forced rebuild, run once with `-PncvForceGuestRebuild=true`, or set `ncv.forceGuestRebuild=true` temporarily. That flag forces the selected artifacts to rebuild during every Android Studio build while it remains true; switch it back to `false` afterward.

## Android images are separate runtime inputs

Gradle deliberately does **not** build or embed these AOSP outputs:

- `boot.img`
- `init_boot.img`
- `vendor_boot.img`
- `system.img` / `super.img`
- `vendor.img`

You build these from the matching AOSP configuration and select them in NCV at runtime. They are not copied into the APK by these tasks. This avoids duplicating large, device/build-specific images inside the application and keeps them user-supplied.

For a Play release, a diagnostic kernel/initramfs can increase APK/AAB size. Set `ncv.packageGuestArtifacts=false` (or build with `-PncvPackageGuestArtifacts=false`) to omit those optional bring-up assets. `libncv.so` and the app still build normally.

## Gradle tasks to run manually

- `:app:verifyNcvNativeSources` — check the native source list.
- `:app:buildNcvDiagnosticKernel` — optional Linux `Image` build.
- `:app:buildNcvStaticBusyBox` — optional static ARM64 BusyBox build.
- `:app:buildNcvTestInitramfs` — optional test initramfs packaging.
- `:app:prepareNcvApkAssets` — prepare manifest/board/guest assets.
- `:app:assembleDebug` — build the debug APK; the pre-build hook connects the steps automatically.

Successful compilation/package steps do not prove that the NCV emulator can boot the diagnostic Linux kernel or Android. A real UART log is still required.
