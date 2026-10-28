# NCV durum ve kalan kilometre taşları

Durumlar: **HOST KODU/DERLEME VAR** = kaynak derlendi veya host üzerinde kontrol edildi; **YAZILDI · CİHAZDA DOĞRULANMADI** = uygulama/kod mevcut fakat end-to-end kanıt yok; **EKSİK** = özellik hâlâ yok. Test/derleme başarısı gerçek OS boot başarısı değildir.

| Aşama | Güncel durum | Not |
|---|---|---|
| 0 Keşif/tasarım | Kısmen | Mimari ve uyumluluk dokümanları var; NCV board sözleşmesi deneysel v0 seviyesinde. |
| 1 Native iskelet + JNI | Yazıldı · cihazda doğrulanmadı | JNI/native bridge var. Android Gradle/NDK APK derlemesi ve fiziksel cihaz kurulum testi bu çalışma oturumunda yapılmadı. |
| 2 ARM64 CPU | Kısmen | Integer/FP/SIMD komutlarının geniş bir alt kümesi; tam ARM64 ISA/feature emülasyonu değil. |
| 3 Guest bellek/MMU | Kısmen | Bus/MMIO, 4 KiB stage-1 MMU/TLB'nin sınırlı yolu var; tam mimari doğrulama ve tüm fault/attribute davranışları yok. |
| 4 EL/system/exception | Kısmen | EL0/EL1, SVC/ERET ve bazı yazmaçlar; eksik sistem yazmaçları, exception/interrupt semantiği, GIC ve timer davranışları gerçek Linux ile doğrulanmadı. |
| 5 UART + bare-metal guest | Host seviyesinde kısmen kanıtlı | Sentetik bare-metal test yükü UART'a yazabilir. Bu, gerçek Linux kernel boot demek değil. |
| 6 Gerçek Linux kernel boot | **YAZILDI · DOĞRULANMADI** | `Image`/`Image.gz`, DTB ve artık isteğe bağlı `initramfs.cpio.gz` yükleme/boot denemesi var. İlk gerçek kernel ve `/init` çıktısı hâlâ gerekli. Kernel-config fragment/script eklendi. |
| 7 Virtio blok depolama | Yazıldı · end-to-end doğrulanmadı | Raw ve Android sparse imajları virtio-mmio üzerinden sunma yolu ve app-private writable raw disk bağlantısı mevcut. Guest fstab ve gerçek kernel sürücüsüyle doğrulama yok. |
| 8 Android boot container | Parser/loader var; Android boot yok | `boot.img` v0–v4, `vendor_boot.img` v3/v4 ve opsiyonel `init_boot.img` parse/initramfs birleştirme yolu var. CPU boot, board uyumu ve Android userspace doğrulanmadı. |
| 9 GSI/dynamic partitions | Kısmen | UI `system.img` veya `super.img` seçebilir. Android `super` metadata'sını mantıksal `system/vendor/product` block device'larına dönüştürme akışı yok; GSI boot yok. |
| 10 Görüntü/giriş/ağ/performance | EKSİK | UART konsolu var; Android frame buffer/GPU/HWC/gralloc/SurfaceFlinger ve guest input yolu yok. Ağ da tam guest cihazı seviyesinde yok. |
| 11 Güvenlik/paketleme | Kısmen / doğrulanmadı | URI temelli dosya erişimi, sınırlı dosya boyutu ve bazı parser sınır kontrolleri var. Android APK güvenlik incelemesi, AVB politikası, Google Play test ve fiziksel cihaz doğrulaması yapılmadı. |

## Yeni eklenen bring-up araçları

- `guest-board/ncv_linux_bringup.config`: ilk UART'lı ARM64 Linux testi için çekirdek konfigürasyon parçası.
- `tools/build_ncv_kernel.sh`: kullanıcının kendi Linux kernel source tree'sinden `Image` üretmeyi dener ve gerekli temel config seçeneklerini kontrol eder.
- `tools/make_ncv_test_initramfs.sh`: statik AArch64 BusyBox ile test `initramfs.cpio.gz` paketler.
- `GuestConsoleActivity`: Linux kernel seçildiğinde otomatik çalıştırmak yerine initramfs seçme ve açıkça başlatma UI'si sunar.
- `docs/OWNER_SETUP_AND_BOOT_GUIDE_TR.md`: geliştirici tarafının yapacağı işler ve Android boot dosyaları için Türkçe kılavuz.

## Hâlâ gerçek Android ekranını engelleyenler

1. Android Gradle/NDK APK build + fiziksel cihaz testinin kullanıcı makinesinde doğrulanması.
2. NCV'nin yazılım CPU modelinde gerçek Linux kernel'i boot edip initramfs `/init` noktasına ulaşmak; eksik ARM64 instruction/system-register/exception davranışını gerçek kernel fault çıktıları üzerinden tamamlamak.
3. NCV virtual board ile uyumlu bir Android kernel + generic/vendor ramdisk + fstab + bootconfig + vendor HAL/userspace üretmek. Fiziksel MediaTek telefonun boot/vendor imajları NCV board'a otomatik uymaz.
4. Android `super` metadata/logical partition eşleme, AVB/verity ve kalıcı `/data` ilk kurulum/mount yaşam döngüsü.
5. Android uyumlu görüntü aygıtı ve guest graphics/input integration.
6. Bir Android build için uçtan uca GSI boot'u kanıtlamak; ondan sonra sürüm matrisi oluşturmak.

Ayrıntı: `docs/OWNER_SETUP_AND_BOOT_GUIDE_TR.md`, `guest-board/README.md`, `docs/ANDROID_GSI_BOOT_GUIDE.md`.
