# NCV Hybrid DTB Bring-up (27 Ekim 2026)

## Bu yama neden var?

Son misafir günlüklerinde AArch64 `B .` (`0x14000000`) komutunun aynı PC'de tekrarlandığı görüldü. Bu, CPU'nun `UnsupportedInstruction` ile durduğu anlamına gelmez; misafir kodu bir sonsuz bekleme/duraklama yoluna girmiştir. Tek başına bu kayıt, bekleme döngüsünün nedenini söylemez.

Bir diğer önemli risk kaynakta bulundu: `boot.img` v2 içindeki DTB ayrıştırılıp saklanabiliyor, ancak önceki akışlarda bazı yollarda NCV'nin genel DTB'si kullanılıyordu. Telefonun MTK kernel'i, fiziksel kartın DTB'sine bağlı clock/reset ve platform düğümlerine ihtiyaç duyabilir. Bu yama, MTK probe profilinde `boot.img` içindeki DTB'yi NCV'nin sanal kartına uyarlamak için deneysel bir yol ekler.

## Bu sürümdeki değişiklikler

- Android `boot.img` v2'nin gömülü DTB/DT table yükü çıkarılır ve boyutu boot konsolunda gösterilir.
- MTK probe profili + gömülü DTB varsa `buildHybridAndroidDtb()` kullanılır. Konsolda `MTK hybrid DTB selected` ve `Hybrid boot DTB: source=...` satırları görülmelidir.
- Bellek aralığı NCV'nin guest RAM alanına uyarlanır; GICv2, PL011, ARM timer, RTC, virtio-mmio disk/RNG ve framebuffer düğümleri sanal kart adreslerine göre yazılır.
- Kaynak DTB'nin fiziksel SoC'sine ait `/reserved-memory` çocukları ve guest RAM'le çakışan FDT reserve-map kayıtları kaldırılır. Stale `memory-region` referansları da kaldırılır.
- Eski MTK MMIO prob aralığı `0x10000000..0x20000000` idi ve framebuffer da `0x10000000` adresinden başlıyordu. Bu çakışma, o bölgedeki fiziksel MMIO erişimlerinin probe shim yerine framebuffer'a gitmesine yol açabilirdi. NCV framebuffer `0x3F000000` adresine taşındı.
- Engine build ID: `0.1.3-dev-hybrid-dtb-20261027`.
- Misafir çalışma anı snapshot'ı X0-X3, SCTLR_EL1, ESR_EL1, FAR_EL1, EL ve aynı PC/talimat dilimlerini raporlar. `B .` döngüsü görülürse `suspected_spin_loop=1` eklenir.
- Test: Android boot/DTB için seçili host testleri ve MTK probe testi çalıştırıldı: 12/12 geçti. C++ motor statik kütüphanesi derlendi. Tam Android APK derlemesi ve fiziksel cihaz boot'u bu ortamda doğrulanmadı.

## Android Studio'da uygulama

Bu paket, mevcut Android Studio projesine kaynak yaması olarak uygulanmalıdır. Kendi `local.properties`, `gradlew`, `gradle/wrapper`, imza ve yerel SDK ayarlarını koru.

1. NCV uygulamasını tamamen kapat ve proje klasörünün yedeğini al.
2. `ncv_hybrid_dtb_patch.zip` içindeki dosyaları proje köküne aynı göreli yollarla kopyala; mevcut kaynakların üzerine yaz.
3. Android Studio'da **Sync Project with Gradle Files** çalıştır.
4. Terminalde:

   ```bash
   ./gradlew --stop
   rm -rf app/.cxx app/build
   ./gradlew :app:assembleDebug --rerun-tasks
   ```

5. Engine sürümünü APK'den kontrol et:

   ```bash
   ./tools/verify_ncv_hybrid_dtb_apk.sh app/build/outputs/apk/debug/app-debug.apk
   ```

6. Kur:

   ```bash
   adb install -r app/build/outputs/apk/debug/app-debug.apk
   ```

   Uygulamayı kaldırıp tekrar kurma; uygulama özelindeki kalıcı `userdata` dosyasını korumak istiyoruz.

## Yeni denemede logda beklenenler

- `Native engine build: 0.1.3-dev-hybrid-dtb-20261027`
- `Embedded boot.img DTB: ... available`
- `MTK hybrid DTB selected: ...`
- `Hybrid boot DTB: source=raw FDT` veya `source=Android DT table...`
- `Runtime snapshot` içinde `x0`, `x1`, `x2`, `x3`, `sctlr_el1`, `esr_el1`, `far_el1`, `EL`, `same_pc_slices` alanları.

`No embedded boot.img DTB` veya `falling back to generic NCV DTB` görülürse hibrit yol devreye girmemiş demektir. Yeni engine ID görünmüyorsa yeni native kütüphane APK'ye girmemiştir veya eski APK çalışıyordur.

## Hâlâ neler garanti değil?

Bu hibrit DTB, gerçek MT6768 SoC emülatörü değildir. Vendor DTB'nin fiziksel clock/reset, PMIC, IOMMU, storage, Mali GPU ve özel HAL gereksinimlerini tamamlamaz. Fiziksel `/reserved-memory` havuzlarını kaldırmak sanal RAM haritasını daha tutarlı yapar ama ilgili vendor sürücülerini çalıştırmaz. `super.img` dosyası virtio disk olarak görünse bile Android first-stage init'in fiziksel `/dev/block/by-name/...` beklentileri, dynamic partition / device-mapper / AVB ve vendor fstab yolları ayrıca ele alınmalıdır.

Bu sebeple patch için başarı ölçütü “Infinix açıldı” değil; ilk olarak uyarlanmış DTB'nin gerçekten kullanılması ve kernel'in yeni bir PC/boot aşamasına geçmesidir.

## `B .` döngüsünü sembolleştirme

Aynı `vmlinux` kernel build'ine ait semboller varsa:

```bash
./tools/symbolize_ncv_pc.sh /path/to/matching/vmlinux 0xffffff80080894a8
```

`vmlinux` tam olarak `boot.img` içindeki kernel ile eşleşmelidir. Başka bir genel AOSP kernelinin `vmlinux` dosyası fiziksel Infinix vendor kernel'inin PC'sini doğru sembolleştirmez. Eşleşen debug sembolleri yoksa kernel'i sembollerle yeniden üretmek gerekebilir.

## Desteklenen yaklaşım

AArch64 talimat emülasyonu cihazdan bağımsız ve ortak olmalı. Donanım/DTB profili SoC veya sanal kart sınıfına göre değişebilir. Genel GSI'leri geniş cihaz kitlesinde çalıştırmak için en ölçeklenebilir yol NCV'nin tanımlı `virt` benzeri kartına uygun Android kernel + GSI üretmektir. Fiziksel Infinix stock ROM'unu bu karta getirmek ise gerçek MTK platform sürücülerinin ve vendor userspace beklentilerinin ayrıca çözülmesini gerektirir.
