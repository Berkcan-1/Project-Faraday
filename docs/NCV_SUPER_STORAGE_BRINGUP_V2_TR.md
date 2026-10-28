# NCV Super Storage Bring-up V2

## Neden OEM boot.img hâlâ siyah ekranda?

Son logda CPU instruction retirement devam ediyor; ancak 20 saniye sonra bütün virtio blok aygıtlarında `mmio_reads=0`, `read_requests=0` ve seri konsol çıktısı yok. NCV'nin mevcut kartı PL011 + GICv2 + virtio-mmio sunuyor. Infinix/MediaTek OEM kernel'i ise kendi fiziksel kartı ve sürücüleri için derlenmiş. Device tree içine `virtio,mmio` düğümü eklemek, kernel'de `CONFIG_VIRTIO_MMIO` ve `CONFIG_VIRTIO_BLK` sürücülerini yoktan oluşturmaz. `super.img` kernel içermez; kernel ve ramdisk ayrı boot bileşenindedir.

## Bu kaynak paketindeki gerçek sonraki adım

ROM Workspace'te `super.img` seçiliyken `Boot NCV kernel + super.img storage` düğmesi görünür. Bu yol OEM `boot.img` kernel'ini bilerek kullanmaz. Gradle tarafından üretilmiş NCV-uyumlu genel ARM64 Linux `Image` ve BusyBox initramfs kullanır. Seçilen `super.img` read-only GPT carrier olarak eklenir; liblp tablosundaki uygun `system_a`, `vendor_a`, `product_a` ve `system_ext_a` gibi bölümler ayrıca read-only host-side extent görünümleri olarak eklenir.

BusyBox `/init`, `/proc/partitions` ve `/dev/vda`, `/dev/vda1`, `/dev/vdb` vb. düğümleri listeler, her mevcut diskten 512 baytlık gerçek bir okuma dener ve `[NCV-GUEST] BLOCK_READ_OK /dev/vda` benzeri mesaj üretir. Bu, misafir kernelin gerçekten UART ve blok aygıtlarını kullandığını ispatlamaya yöneliktir. Bu aşama Android `init`, `dm-linear`, ART, Binder, SurfaceFlinger veya boot animation başlatmaz.

## Build gereksinimleri

- `guest-board/ncv_linux_bringup.config` kaynak paketine dahil olmalı.
- `local.properties` içinde `ncv.linuxKernelSource` ve `ncv.busyboxSource` yerel kaynak ağaçlarını göstermeli.
- `aarch64-linux-gnu-gcc`, kernel build araçları, `cpio`, `gzip` ve statik ARM64 BusyBox gerekli.
- Android APK'nın native engine satırı `0.1.18-dev-ncv-gzip-asset-name-fix-20261028` olarak görünmeli. Eski `0.1.12` veya önceki `0.1.13` görünüyorsa güncel kaynaklarla derlenen APK kurulu değildir.

## Beklenen doğrulama

1. `[NCV-GUEST] INITRAMFS /init STARTED`: Linux kernel `/init` sürecini başlattı.
2. `[NCV-GUEST] BLOCK_READ_OK /dev/vda`: guest virtio-blk aygıtı üzerinden gerçek okuma tamamlandı.
3. `[NCV-GUEST] BLOCK_READ_OK /dev/vda1`: kernel GPT taşıyıcısındaki `super` bölümünü tanıdı ve okuyabildi.
4. `/dev/vdb` vb. okuma mesajları: NCV host-side mantıksal extent görünümleri guest kernelden okunuyor.

Bu kontroller geçmeden Android `init` ve UI'ye geçiş doğrulanmış sayılmaz. OEM boot kernel'ini generic kartta çalıştırmak istiyorsak MT6768/MT6769 SoC ve ilgili storage/display/interrupt donanımını daha kapsamlı emüle etmek ya da hedeflenen NCV board için uyumlu Android kernel/ramdisk/vendor yapılandırması üretmek gerekir.

## Yeni APK'yi eski sürümden ayırt etme

Kullanıcının paylaştığı 0.1.12 logu `HYBRID_VENDOR_DTB` seçildiğini ve tüm virtio `mmio_reads`/`read_requests` sayaçlarının sıfır kaldığını gösteriyordu. Bu kaynak paketi native engine marker'ını `0.1.18-dev-ncv-gzip-asset-name-fix-20261028` yapar, tanı varlıkları yoksa düğmeyi devre dışı bırakır ve varsayılan olarak eksik varlıklarla APK derlemesini engeller.

APK oluşturulduktan sonra:

```bash
./tools/verify_ncv_super_bringup_apk.sh app/build/outputs/apk/debug/app-debug.apk
```

Bu doğrulayıcı, native sürüm marker'ının yanı sıra `assets/ncv/guest/diagnostic/Image` ve `assets/ncv/guest/diagnostic/initramfs.cpio.gzip` dosyalarının APK'de gerçekten bulunduğunu denetler. Marker veya varlıklar yoksa eski/eksik APK çalıştırılmamalıdır.

## Paketleme / Gradle notu

Bu kaynak anlık görüntüsünde kök `settings.gradle.kts` ve wrapper özellikleri tamamlandı. Anlık görüntüde `gradle/wrapper/gradle-wrapper.jar` yoktu ve bu çalışma ortamında Android SDK ile yerel Linux/BusyBox kaynak ağaçları da bulunmuyor; bu nedenle burada debug APK üretilemedi. Tam derleme, geliştiricinin kendi makinesindeki Gradle Wrapper JAR'ı veya kurulu Gradle 8.13, Android SDK/NDK, Linux kernel kaynak ağacı, BusyBox kaynak ağacı ve AArch64 cross-compiler ile yapılmalıdır. `tools/verify_ncv_super_bringup_apk.sh` APK'nin beklenen build marker'ını ve tanı varlıklarını doğrular.


## v0.1.15 varlık kilidi

Varsayılan olarak `ncv.requireDiagnosticGuestArtifacts=true` ayarı eksik kernel/initramfs varlıklarıyla APK oluşturulmasını engeller. `tools/build_verify_ncv_super_bringup.sh` derleme sonrası native motor işaretini, iki varlığın SHA-256 ve boyutlarını, ARM64 Image magic alanını ve initramfs içindeki `/init` işaretini doğrular. Çalışan uygulamada tanı boot düğmesi yalnızca iki varlık APK içinde okunabiliyorsa etkin olur.
