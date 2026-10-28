# Stage 4C/4D host verification

Updated: 2026-10-26

Result: **87 tests passed, 0 failed** on the Linux host build.

Implemented development slices in this snapshot:
- Limited single-vCPU GICv2 distributor/CPU interface; interrupt acknowledge, EOI, pending/active, priority, target and configuration register windows, plus level-trigger re-pending. PL011 RX interrupt line is connected to GIC SPI 33.
- Host-monotonic generic counter plus the common physical/virtual timer register subset and timer IRQ delivery at instruction boundaries.
- Conservative partial ARM64 feature-ID/system-register profile.
- FDT v17 generator for RAM, CPU, PL011, GICv2, timer, bootargs and optional initrd location.
- ARM64 Linux `Image` header validation and loader for raw or gzip-compressed little-endian `Image`. The gzip path validates stream completion and is capped at 512 MiB after decompression. The loader checks Image magic, size/flags, RAM bounds, kernel-page profile and load placement, then selects the EL1 entry point.
- Kotlin/JNI bridge helpers for loading a raw/gzip Image and preparing a DTB/X0 handoff.
- The initial stack grows into a 64 KiB reserved gap immediately below the DTB; the helper rejects known kernel/initrd overlap with that gap and DTB.

Verified in this run:
- Existing Stage 1–4B host regression suite.
- GICv2 limited register programming, IAR/EOI and level re-pending.
- PL011 RX FIFO → GIC SPI 33 interrupt delivery and deassertion after guest reads DR.
- Virtual timer IRQ delivery at an instruction boundary.
- Feature ID and `CNTFRQ_EL0` reads at EL1.
- FDT header, basic node strings, DTB/X0 handoff.
- Synthetic ARM64 raw/gzip Image header validation, placement at `RAM base + text_offset`, EL1 entry selection and DTB handoff.
- Invalid/missing Image magic and unsupported 16 KiB kernel-page flag rejection.
- JNI translation unit syntax check with host JDK headers and Kotlin bridge compilation.

Host builds require the zlib development package; Android NDK builds link the platform `libz`.

Host core build/test command:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
./build/ncv_tests
```

JNI/Kotlin source checks performed:

```sh
g++ -std=c++17 -Wall -Wextra -fsyntax-only \
  -Iapp/src/main/cpp \
  -I/usr/lib/jvm/java-21-openjdk-amd64/include \
  -I/usr/lib/jvm/java-21-openjdk-amd64/include/linux \
  app/src/main/cpp/jni/ncv_jni.cpp

kotlinc app/src/main/java/com/aerolon/ncv/NativeBridge.kt \
  app/src/main/java/com/aerolon/ncv/NcvVm.kt -d /tmp/ncv-kotlin-stage4cd.jar
```

## Explicitly not verified

- The direct-boot tests use a **synthetic Image with HLT instructions**. It proves parser/placement/handoff behavior, **not a Linux kernel boot**.
- No real ARM64 Linux `Image` was booted; no Android GSI boot was attempted.
- Android `boot.img`/`vendor_boot` parsing, bootconfig, AVB/vbmeta, initramfs/vendor ramdisk, dynamic partitions, `super.img`, block storage/virtio and Android guest services remain unimplemented.
- No Android SDK/NDK was available in this environment. No Android APK build/install/runtime test was performed.
- No Google Play or Huawei AppGallery review/compliance verification was performed.

The raw Linux Image format checks follow the public ARM64 Linux boot protocol: <https://docs.kernel.org/arch/arm64/booting.html>.
