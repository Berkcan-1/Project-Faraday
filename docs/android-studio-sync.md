# Android Studio setup for NCV UI prototype

This source snapshot contains Android app and native CMake build configuration but does not bundle the Gradle wrapper JAR/scripts or Android SDK/NDK binaries.

## Recommended setup

- Android Studio with a local Gradle 8.13 distribution (AGP 8.13.2 is configured).
- Android SDK Platform 36 and Build Tools selected by SDK Manager.
- Android NDK and CMake 3.22.1.
- Java 17 toolchain.

Open `ncv/` as a Gradle project. If the IDE reports that it cannot locate a Gradle distribution, select/install Gradle 8.13 or start from Android Studio's **Native C++** template and copy `app/src/main/` from this source archive, retaining the package `com.aerolon.ncv`. Configure the generated app module with the `android { ... }` settings from `app/build.gradle.kts` and the CMake path `src/main/cpp/CMakeLists.txt`.

## Verify before installing

1. Run Gradle Sync.
2. Run `:app:assembleDebug`.
3. Install only on an ARM64 Android device (or add another ABI after verifying the native engine on it).
4. Select a known Linux ARM64 `Image` only to test the experimental serial-console path. A GSI/system partition must show the explicit unsupported diagnostic until the missing engine subsystems are implemented.

A successful Gradle build proves packaging/compilation only. It does not prove a Linux kernel boot or Android GSI compatibility.


## Gradle ile yardımcı misafir artefaktlarını otomatik derleme

C++ motoru Android'in normal NDK/CMake işlemiyle zaten derlenir. İsteğe bağlı tanılama Linux kernel'i ve test initramfs'ini de Gradle build aşamasında otomatik üretmek için [`GRADLE_AUTOMATION.md`](GRADLE_AUTOMATION.md) dosyasını izleyin. Bunun için `local.properties` içine standart bir Linux kernel kaynak yolu ve BusyBox kaynak yolu eklenir. Bu ek görevler Android AOSP `boot.img`, `init_boot.img`, `vendor_boot.img`, `system.img` veya `super.img` üretmez; bu imajlar kullanıcı tarafından AOSP'den hazırlanıp uygulama içinde seçilir.
