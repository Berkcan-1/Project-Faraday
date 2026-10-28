# Android Studio entegrasyonu (DOĞRULANMADI: bu ortamda NDK/Android Studio yoktu)

1. `app/src/main/cpp/` klasörünü projenin aynı yoluna kopyala.
2. `app/src/main/java/com/aerolon/ncv/*.kt` dosyalarını kopyala. Paket adını değiştirirsen `ncv_jni.cpp` içindeki
   `Java_com_aerolon_ncv_NativeBridge_*` sembollerinin hepsini yeni pakete göre yeniden adlandır, yoksa `UnsatisfiedLinkError` alırsın.
3. `app/build.gradle.kts` (sürümleri kendi SDK Manager'ına göre ayarla):
```kotlin
android {
    defaultConfig {
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild { cmake { cppFlags += "-std=c++17" } }
    }
    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt") } }
}
```
4. İlk kontrol (arka plan thread'inde çağır): `Log.i("NCV", NativeBridge.nativeSelfTest())`. Beklenen ilk satır: `SELFTEST PASS`.
5. Kullanım örneği:
```kotlin
NcvVm.create(16, initialEL = 0).use { vm ->
    vm.loadPayload(helloBytes)   // 0x40000000'a yükler
    vm.start()
    // state() == STOPPED olunca vm.console() -> "NCV GUEST BOOT OK\n"
}
```
`initialEL=0` uyumluluk varsayılanıdır. Kernel benzeri guest payload’ları için `initialEL=1` seçilebilir; bu yalnızca CPU başlangıç seviyesini ayarlar, MMU/Android boot desteği sağlamaz.

Not: `nativeSelfTest` ve `pause()` kısa bloklar; UI thread'inde uzun süre beklemeyecek şekilde yine de arka planda çağırmanı öneririm.

## Raw ARM64 Linux Image API (kernel direct-boot bring-up only)

`NcvVm.loadLinuxImage(image)` accepts a raw or gzip-compressed little-endian AArch64 Linux `Image` with a valid standard header. Gzip expansion is capped at 512 MiB. It does not parse Android `boot.img` or `vendor_boot`, and rejects unsupported kernel page-size flags. Load the kernel before preparing the DTB. If a generic initrd is used, load its bytes into guest RAM with `loadPayload(data, address)` and pass the exact range to `prepareLinuxBootDtb(...)`.

This API has only been tested with a synthetic Image that immediately halts; it has **not** booted a real Linux kernel. Android GSI needs additional Android boot-image/ramdisk/partition support and is not supported by this snapshot.
