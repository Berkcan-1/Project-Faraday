# NCV Super Storage Bring-up v0.1.15

## Önceki siyah ekranın nedeni

Paylaşılan son logda native engine `0.1.12-dev-lazy-ram-boot-gates-20261028` idi ve çalışma `boot.img` içindeki Infinix kernel'ini çalıştırmaya çalışıyordu. Bu log `NCV diagnostic Linux` yolunun çalıştığını göstermiyor. Üstelik 20 saniye sonunda UART, VirtIO MMIO erişimi ve framebuffer yazımı sıfırdı. Böyle bir oturumda `super.img` içeriğine henüz guest kernel tarafından erişildiği söylenemez.

## v0.1.15 değişiklikleri

- Native engine build marker: `0.1.18-dev-ncv-gzip-asset-name-fix-20261028`; Android `versionCode=3`, `versionName=0.1.15-dev`.
- Diagnostic boot asset gate eklendi. `ncv.requireDiagnosticGuestArtifacts=true` varsayılanı ile gerçek ARM64 Linux `Image` ve BusyBox initramfs olmadan Gradle APK build'i başarısız olur.
- UI, APK içindeki `ncv/guest/diagnostic/Image` ve `ncv/guest/diagnostic/initramfs.cpio.gzip` dosyalarını yoklar. Bunlar eksikse NCV diagnostic boot düğmesi disabled olur; kullanıcıya missing-asset açıklaması gösterilir. Eski UI kodunun düğmeyi daha sonra tekrar etkinleştirmesi de düzeltildi.
- Kernel build config `CONFIG_SERIAL_EARLYCON=y` ile UART erken konsolunu talep eder ve build script bunun çözüldüğünü doğrular.
- BusyBox `/init` ilk satırda UART marker'ı basar, ardından devtmpfs/proc/sysfs/tmpfs mount durumlarını raporlar; böylece mount hatası olsa bile `/init` çalışıp çalışmadığı anlaşılır.
- `tools/build_verify_ncv_super_bringup.sh` derlemeyi ve APK doğrulamasını tek komutta yapar. `--install` argümanı, APK doğrulandıktan sonra adb ile yüklemeye çalışır.
- `tools/verify_ncv_super_bringup_apk.sh` build marker'ını, manifestteki boyut ve SHA-256 değerlerini, raw ARM64 Image magic'ini ve initramfs içindeki `/init` içeriğini doğrular.

## Derleme ve kurulum

1. `local.properties` içindeki `ncv.linuxKernelSource` ve `ncv.busyboxSource` yollarının gerçekten kernel ve BusyBox kaynak ağaçlarına işaret ettiğini doğrula.
2. `aarch64-linux-gnu-gcc`, `make`, `cpio`, `gzip`, `readelf` ve Android SDK/NDK kurulu olmalı. Android Studio'nun Gradle/SDK ayarları kullanılabilir.
3. Proje kökünde `tools/build_verify_ncv_super_bringup.sh` çalıştır. Gradle Wrapper JAR eksikse script sistemde veya Gradle cache'inde bulunan Gradle 8.13'ü arar; bulunamazsa Android Studio'dan `:app:assembleDebug` çalıştır.
4. APK doğrulaması geçtikten sonra `tools/build_verify_ncv_super_bringup.sh --install` kullanılabilir (ADB ve imzalama anahtarı uyumu gerekir).
5. Uygulamada ROM Workspace'e gir, `super.img` seç ve **Boot NCV diagnostic Linux + super.img** düğmesine bas. Bu eylem OEM `boot.img` içindeki kernel'i kullanmaz. Eski/deneysel **Attempt OEM kernel** düğmesi bu depolama bring-up testinin yerine geçmez.

## Başarı koşulları

Logda sırayla şu gözlemleri arıyoruz:

1. `[NCV-GUEST] INITRAMFS /init STARTED`: generic ARM64 Linux kernel initramfs `/init` sürecini başlattı.
2. `/proc/partitions` içinde virtio diskleri görünür.
3. `[NCV-GUEST] BLOCK_READ_OK /dev/vda` veya ilgili bölüm aygıtı: misafir kernel blok sürücüsüyle gerçek okuma yapabildi.
4. `mmio_reads` ve `read_requests` sıfırdan büyük olur.

Bunlar geçse bile Android'in açıldığı anlamına gelmez. Bir sonraki aşama gerçek Android boot ramdisk'ine uygun liblp/dm-linear, fstab, AVB/verity ve init yapılandırmasıdır. Infinix'in kendi kernel'i NCV'nin sanal cihazlarını tanımıyorsa generic NCV diagnostic kernel ile depolamayı doğrulamadan bu adıma geçmek doğru olmaz. Grafik/SurfaceFlinger ve boot animation halen ayrı bir emülasyon aşamasıdır.

## Test durumu

Bu geliştirme oturumunda host Release CMake testleri: 135 test, 0 başarısız, 0 kontrol hatası. Asset-gate statik testi ve APK doğrulama scripti sahte test APK'sı ile kontrol edildi. Android SDK ve AArch64 kernel/BusyBox kaynak ağaçları bu çalışma ortamında bulunmadığından gerçek APK derlemesi ya da gerçek guest boot testi yapılmadı.
