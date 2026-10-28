> Android Studio'nun C++ motorunu ve isteğe bağlı misafir Linux kernel/initramfs çıktılarını otomatik derleyip APK'ye paketlemesi için ayrıca [`GRADLE_AUTOMATION.md`](GRADLE_AUTOMATION.md) rehberini kullan. AOSP'den ürettiğin Android `boot.img`, `init_boot.img`, `vendor_boot.img` ve `system.img` dosyaları bu görevler tarafından üretilmez; uygulama içinde çalışma zamanında seçilir.

# NCV — Geliştirici Kurulumu, Linux Bring-up ve Android İmajları

Bu belge, uygulamayı yerel makinede derleyip ilk gerçek ARM64 Linux kernel denemesini yapacak geliştirici için hazırlandı. **Bu pakette gerçek Linux veya Android boot başarısı doğrulanmış değildir.** Kod, boot denemesini mümkün kılacak deneysel yükleme yolunu sağlar; eksik ARM64 davranışları yüzünden gerçek kernel hâlâ erken aşamada takılabilir.

## 1. Önce terimleri ayıralım

| Dosya | Ne içerir? | NCV geliştirirken nasıl düşünülmeli? |
|---|---|---|
| `Image` | Ham ARM64 Linux kernel'i | İlk gerçek kernel bring-up testi için kullanılacak. NCV'nin oluşturduğu DTB ile beraber yüklenir. |
| `initramfs.cpio.gz` | İlk açılışta RAM'e konulan küçük root filesystem (`/init`, BusyBox, araçlar) | Kernel'in UART'a çıktı verdikten sonra bir kullanıcı süreci çalıştırabildiğini doğrulamak için kullanılır. Android ramdisk'iyle aynı şey değildir. |
| `boot.img` | Android boot container'ı; kernel ve Android cihaz/ sürüm düzenine göre generic ramdisk içerebilir | Android boot denemesinde kernel'in geldiği dosya. NCV'nin hedeflediği sanal kart için üretilmiş olması gerekir. |
| `vendor_boot.img` | Modern GKI düzeninde vendor ramdisk parçaları, DTB ve ilgili boot bilgileri | **`vendor.img` ile aynı şey değildir.** Donanıma/board'a özel erken boot dosyalarını taşır. `boot.img` ile aynı Android/board build'inden gelmelidir. |
| `init_boot.img` | Android 13 ile piyasaya çıkan bazı cihaz düzenlerinde generic ramdisk'i taşıyan ayrı bölüm | İçindeki şey `init` adlı tek program değil, generic ramdisk'tir. Yalnızca kullanılan build ramdisk'i buraya ayırdıysa gerekir. Android 12 cihazlarında çoğu zaman ayrı değildir; Android 13'e yükseltilmiş her cihazda da otomatik olarak bulunmaz. |
| `system.img` | Android framework ve sistem dosyaları olan system bölümü / GSI | Tek başına kernel değildir ve boot edilemez. |
| `vendor.img` | Android vendor userspace dosyaları, HAL'ler ve cihaz konfigürasyonları | `vendor_boot.img` yerine kullanılamaz. Sanal kart için uyumlu HAL/vendor derlemesi gerekir. |
| `super.img` | `system`, `vendor`, `product`, `system_ext`, `odm` gibi mantıksal bölümlerin metadata ve içeriklerini tutan dinamik bölüm kapsayıcısı | Ayrı `system.img` yerine seçilebilir; ama Android'in first-stage init / `dm-linear` mantıksal bölüm oluşturma akışı ayrıca kurulmalıdır. |
| `userdata.img` | Android'in kalıcı kullanıcı verileri için yazılabilir bölüm | NCV'nin oluşturduğu 1 GiB app-private disk şu an sadece bir sanal blok diskidir. Doğru fstab, dosya sistemi ve Android mount akışı olmadan kendiliğinden `/data` olmaz. |

### Android 12, 13 ve sonrası: pratik kural

- Android 12/GKI düzeninde generic ramdisk `boot.img` içinde bulunabilir.
- Android 13 ile piyasaya çıkan bazı GKI cihazlarda generic ramdisk `init_boot.img` içine taşınır ve `boot.img` esasen kernel taşır.
- Android 13'e yükseltilmiş eski bir cihaz eski bölüm düzenini koruyabilir; sürüm numarasından tek başına karar verilmez.
- `vendor_boot.img`, GKI düzeninde vendor ramdisk ve çoğunlukla DTB'yi taşır; `boot.img` ile birlikte aynı board/build için üretilmelidir.
- NCV için hedef, fiziksel bir MT6768 telefonun boot imajlarını rastgele kullanmak değil, NCV sanal kartıyla uyuşan bir kernel + ramdisk + fstab + vendor userspace üretmektir.

Kaynaklar: [AOSP Generic Boot Partition](https://source.android.com/docs/core/architecture/partitions/generic-boot), [AOSP Vendor Boot Partitions](https://source.android.com/docs/core/architecture/partitions/vendor-boot-partitions), [AOSP Boot Image Header](https://source.android.com/docs/core/architecture/bootloader/boot-image-header).

### Yalnızca `boot.img` ile deneme yapılabilir mi?

Evet, **deneme** yapılabilir: `boot.img` içinde hem ARM64 kernel hem de boş olmayan generic ramdisk varsa NCV'ye `system.img`/`super.img` ve `boot.img` seçip vendor_boot/init_boot olmadan Attempt Android boot diyebilirsin. Parser/loader bu kombinasyonu kabul eder. Bu, ROM'un açılacağı anlamına gelmez; NCV kendi sanal board DTB'sini verir ve OEM kernel'in fiziksel MTK aygıtlarını bulması beklenemez.

AOSP `unpack_bootimg.py` ile gerçek imajı incele. Kernel varsa ama ramdisk boyutu `0` ise monolithic `boot.img` değildir. Böyle bir image için ya hedefe uygun generic ramdisk'i `boot.img` içine koyan özel AOSP ürün/board yapılandırması üretmeli ya da ramdisk'i taşıyan `init_boot.img`/uyumlu boot bundle sağlamalısın. `vendor_boot.img` de vendor ramdisk/DTB, kernel modülleri ve bootconfig taşıyabilir; bunu Little Kernel (LK) üretmek çözmez. LK bir bootloader'dır; Android Linux kernel'inin veya ramdisk'in yerine geçmez.

## 2. Android Studio projesini derle

Bu kaynak paketi `AGP 8.13.2`, Kotlin Gradle plugin `2.2.21`, `compileSdk 36`, NDK ve CMake `3.22.1` hedefliyor. Önce Gradle ile Android Gradle Plugin sürümlerinin uyumlu olduğundan emin ol. AGP 8.13.x için Gradle 8.13 kullan.

Önce proje dizininde `gradlew` bulunup bulunmadığını kontrol et:

```bash
ls -l gradlew gradle/wrapper/gradle-wrapper.properties
```

- **Wrapper varsa:** `gradle/wrapper/gradle-wrapper.properties` içindeki `distributionUrl` değerini `https\://services.gradle.org/distributions/gradle-8.13-bin.zip` yap. Sonra `./gradlew --stop`, `./gradlew --version` ve `./gradlew assembleDebug` çalıştır.
- **Wrapper yoksa:** Bu kaynak ZIP'i wrapper JAR/script dosyalarını içermez. Android Studio'da `Settings > Build, Execution, Deployment > Build Tools > Gradle` bölümünden Gradle 8.13 dağıtımını seç veya mevcut projenin wrapper dosyalarını koruyarak sadece bu paketteki `app/src/main`, `docs`, `tools` ve `guest-board` içeriğini mevcut projene aktar. Android Studio Gradle görev listesinden `:app:assembleDebug` görevini çalıştır.

`--version` çıktısında Gradle `8.13` görmelisin. AGP `8.13.2` ile Gradle `9.6.0` kullanma.

Beklenen APK yolu:

```text
app/build/outputs/apk/debug/app-debug.apk
```

Telefon bağlıyken:

```bash
adb devices
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Bu paketin Android SDK/NDK ortamında derlemesi ve fiziksel cihazda çalışması bu geliştirme oturumunda doğrulanmadı. Derleme hatası alırsan tam ilk hata bloğunu kaydet; bir hata düzeltilmeden sonraki hataları tahmin ederek çözmeye çalışma.

## 3. İlk gerçek kernel denemesi: ARM64 Linux

Bu paket `tools/build_ncv_kernel.sh` scriptini ve `guest-board/ncv_linux_bringup.config` ayar parçasını içerir. Bunlar bir Android kernel'i değil, UART ile görünür ilk Linux denemesini hazırlamak içindir.

Ubuntu'da gerekli araçlar:

```bash
sudo apt update
sudo apt install build-essential bc bison flex libssl-dev libelf-dev \
  gcc-aarch64-linux-gnu cpio gzip git python3 rsync binutils libncurses-dev
```

Linux kernel kaynak kodunu kendin edin (ör. kernel.org'dan kaynak ağacı indir veya git ile klonla). Ardından NCV proje dizininde:

```bash
./tools/build_ncv_kernel.sh "$HOME/src/linux" "$HOME/ncv-kernel-out"
```

Gerekirse çapraz derleyici önekini ve iş parçacığı sayısını değiştir:

```bash
CROSS_COMPILE=aarch64-linux-gnu- JOBS=4 \
  ./tools/build_ncv_kernel.sh "$HOME/src/linux" "$HOME/ncv-kernel-out"
```

Başarılı derleme sonucu:

```text
$HOME/ncv-kernel-out/arch/arm64/boot/Image
```

Script kritik kernel konfigürasyonlarının etkin kaldığını kontrol eder. Kernel sürümündeki Kconfig isimleri/dependencies değişmişse script durabilir; bu durumda `$HOME/ncv-kernel-out/.config` dosyasını ve script hatasını raporla. Build'in başarılı olması, NCV CPU'sunun o kernel'i çalıştırabildiğini kanıtlamaz.

## 4. Deneme initramfs'i hazırla

İlk boot testi için Android `system.img` gerekmez. Statik ARM64 BusyBox ve `/init` içeren küçük bir `initramfs.cpio.gz` kullan.

BusyBox'ı ARM64 için cross-compile et:

```bash
git clone https://git.busybox.net/busybox "$HOME/src/busybox"
cd "$HOME/src/busybox"
make defconfig
make menuconfig
```

`Settings` içinden **Build static binary (no shared libs)** seçeneğini etkinleştir ve kaydet. Sonra:

```bash
CROSS_COMPILE=aarch64-linux-gnu- make -j4
file busybox
```

`file busybox` çıktısı AArch64/ARM aarch64 ve statik executable olduğunu doğrulamalı. Ardından NCV proje dizinine dönüp:

```bash
./tools/make_ncv_test_initramfs.sh \
  "$HOME/src/busybox/busybox" \
  "$HOME/ncv-test-initramfs.cpio.gz"
```

Paketleyici `cpio` ve `gzip` kullanır. Çıktı, Android ramdisk'i değil, sadece `/init` + BusyBox'tan oluşan Linux testi içindir.

## 5. NCV uygulamasında Linux bring-up

1. Uygulamada gerçek ARM64 `Image` dosyasını seç.
2. Açılan **LINUX KERNEL BRING-UP** alanında `Select initramfs.cpio.gz` düğmesine bas ve oluşturduğun initramfs'i seç. Bu adım isteğe bağlıdır, ancak `/init` testini sağlar.
3. `Attempt Linux kernel boot` düğmesine bas.
4. Ekrandaki UART konsolunu izle. Başarı işareti yalnızca “vCPU çalışıyor” değildir: kernel'in gerçek boot mesajlarını ve ardından `[NCV-GUEST] INITRAMFS /init STARTED` mesajını görmemiz gerekir.
5. Eğer VM fault olursa son 100–200 satırlık UART çıktısını, `GUEST FAULTED` ayrıntısını, kullanılan kernel kaynağının sürüm/commit bilgisini ve `.config` dosyasını kaydet.

Bu testte başarısız olmak beklenebilir: NCV'nin ARM64 komutları/sistem yazmaçları/istisnaları ve interrupt/timer emülasyonu henüz tam bir gerçek kernel ile doğrulanmış değil. Kernel'in ilk loguna ulaşmak en önemli sonraki hedef.

## 6. Sonra gerçek Android/GSI'ye geçmek için

İlk Linux boot doğrulanmadan GSI testinden anlamlı sonuç bekleme. Android için geliştirici tarafında daha büyük bir iş gerekir:

1. NCV'nin sanal kart donanım sözleşmesini sabitle: RAM adresi/kapasitesi, PL011 UART, GIC, timer, virtio-mmio adresleri/IRQ'lar ve DTB uyumluluğu.
2. Bu sanal karta uygun bir Android ortak kernel'i/konfigürasyonu üret; NCV'de uygulanmayan CPU talimatları ve system register'ları için loglardan başlayarak eksikleri tamamla.
3. Android ramdisk'ini üret: `init`, udev/devtmpfs beklentileri, fstab, bootconfig ve gerekli modüller bu sanal board'a uygun olmalı. `boot.img`, `vendor_boot.img` ve gerekiyorsa `init_boot.img` aynı build çıktısından gelmeli.
4. Android vendor userspace/HAL üret veya uyumlu bir set hazırla. Fiziksel telefonun MediaTek vendor imajları NCV'nin sanal UART/GIC/virtio kartına otomatik uyumlu değildir.
5. `super.img` metadata'sını okuyup `system`, `vendor`, `product` gibi mantıksal bölümleri doğru block device eşlemeleriyle sun. Yalnızca super dosyasını `/dev/block/vda` olarak takmak Android logical partitions desteği değildir.
6. AVB/verity ve vbmeta politikasıyla uyumlu test imajı üret; geliştirme aşamasında güvenlik kontrollerini sadece gizlice atlatma. Android'in doğrulama gereksinimleri bilerek, açıkça yapılandırılmalı.
7. Yazılabilir `userdata` dosyasını Android'in beklediği dosya sistemi, mount ve ilk kurulum süreciyle birleştir.
8. Grafik için yalnızca framebuffer yeterli olmayabilir: Android userspace'in SurfaceFlinger/HWC/gralloc ve giriş yoluna ihtiyacı vardır. Önce NCV'de basit bir virtio-gpu veya açıkça tanımlanmış sanal görüntü aygıtı ve guest driver stack tasarlanmalı; sonra Android compositor buna bağlanmalı.
9. Android 12, 13, 14, 15, 16, 17 için ayrı test imajları ve uyumluluk matrisi oluştur. Android 18–25 gelecekteki hedefler olarak kalır; şimdiden garanti edilemez.

Android dinamik bölümlerle `super` imajını ele alma süreci first-stage `init` ve kernel device-mapper (`dm-linear`) desteğini gerektirir. Daha fazla bilgi: [AOSP Dynamic Partitions](https://source.android.com/docs/core/ota/dynamic_partitions/implement).

## 7. Bu pakette hazır olan ve olmayan

### Pakette bulunan

- ARM64 `Image` / `Image.gz` yükleme denemesi ve NCV DTB oluşturma.
- Linux kernel ekranında isteğe bağlı `initramfs.cpio.gz` seçme, kernel + initramfs + DTB başlatma girişimi ve UART takibi.
- `boot.img`, `vendor_boot.img`, isteğe bağlı `init_boot.img` ayrıştırma yolu.
- `system.img` / `super.img` / isteğe bağlı `vendor.img` için sanal blok aygıtı bağlama yolu.
- Android profilinde app-private yazılabilir bir `userdata.img` diski oluşturma seçeneği.
- Linux kernel config fragment'i ve Linux `Image`/test initramfs üretmeye yardımcı scriptler.

### Hâlâ doğrulanmamış / eksik

- Android Gradle/NDK ile gerçek APK build ve fiziksel cihaz testi.
- Gerçek bir Linux kernel'in NCV üzerinde başarıyla çalışması.
- Tüm ARM64 CPU/system register/exception davranışları.
- Android logical partition map, tam AVB/verity boot politikası ve `/data` ilk kurulum/yazılabilir veri yaşam döngüsü.
- NCV sanal kartı için tam bir Android kernel, vendor ramdisk, fstab ve vendor userspace/HAL build'i.
- Android grafik/giriş sürücü yolu ve gerçek GSI userspace boot.

**Dürüst başarı ölçütü:** Linux testinde kernel UART mesajı ve `/init` mesajı görünmeden “Linux boot tamam” deme. Android'e ait bir gerçek framework/compositor ekranı gelmeden “GSI boot tamam” deme.

## AOSP neden sadece boot.img üretti?

Hangi `lunch` hedefini kullandığın önemli. Resmî AOSP dokümantasyonuna göre `aosp_arm64` yalnızca GKI üretir; `vendor_boot` ve recovery üretmediği için tam bir cihaz hedefi değildir. Aynı doküman bu yapılandırmalar için `generic_arm64` ürün yapılandırmasına bakılmasını söyler. Ayrı GSI hedefi `gsi_arm64-userdebug` ise `system.img` üretir; GSI'nin kendisi bir bootloader/kernel paketi değildir. Kaynak: [AOSP Generic boot partition](https://source.android.com/docs/core/architecture/partitions/generic-boot) ve [AOSP GSI build targets](https://source.android.com/docs/core/tests/vts/gsi).

NCV denemesi için AOSP ağacındaki imajı şöyle inceleyebilirsin:

```bash
./tools/inspect_android_boot.sh \
  "$ANDROID_PRODUCT_OUT/boot.img" \
  "$ANDROID_BUILD_TOP"
```

Script AOSP `unpack_bootimg.py` aracını çağırır; orijinal `boot.img` dosyasına dokunmaz. Çıktıda `ramdisk` dosyası boş/yoksa bu `boot.img` tek başına NCV'nin beklediği generic initramfs şartını karşılamaz. Bu durumda `init_boot.img` şartını kontrol etmek ya da NCV sanal kartına özel monolithic kernel+ramdisk yapılandırması derlemek gerekir. `vendor_boot.img` eksikliği parser'ın çalışmasını engellemeyebilir ama gerçek kernel için gereken DTB/vendor ramdisk eksik kalabilir.
