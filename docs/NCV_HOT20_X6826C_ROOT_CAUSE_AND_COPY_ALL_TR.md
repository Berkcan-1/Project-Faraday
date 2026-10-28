# NCV — Infinix HOT 20 X6826C Android 12 açılış döngüsü: bulgular ve sonraki adımlar

Tarih: 27 Ekim 2026  
Sürüm: `0.1.9-dev-hot20-boot-diagnostics-20261027`

## Kısa sonuç

Bu paket, stok Infinix Android ROM'unu açtığını iddia eden bir “tam düzeltme” değil. Son kullanıcının gönderdiği PTE-writer logu, bir sonraki teşhis adımını doğrudan gösteriyor. Ayrıca serial console ekranına **Tümünü kopyala** düğmesi ve legacy boot.img fiziksel adres alanlarının raporlanması eklendi.

Gönderilen `ff(2).txt` dosyası kaynak kod değil, tekrarlanan çalışma zamanı logları içeriyor. Değişiklikler önceki proje kaynağına uygulandı.

## 1. Logun kesin olarak gösterdiği şey

Önemli alanlar:

```text
last_abort_va=0xffffff8008025000
last_abort_pa=0x0
last_abort_size=4
last_abort_access=read
last_abort_bus=Unmapped
last_abort_insn=0xb9400101

last_abort_l3_entry_pa=0x5ffcf128
last_abort_pte_writer_found=1
last_abort_pte_writer_pc=0xffffff8009008d48
last_abort_pte_writer_insn=0xf90002b7
last_abort_pte_writer_va=0xffffffc01ffcf128
last_abort_pte_writer_pa=0x5ffcf128
last_abort_pte_writer_value=0x6800000000070f
last_abort_pte_writer_size=8

last_abort_fresh_walk_ok=1
last_abort_fresh_pa=0x0
```

* `0xf90002b7`, `STR X23, [X21]` biçimindeki AArch64 store talimatına karşılık gelir.
* Guest kernel, descriptor değerini `0x5ffcf128` fiziksel adresindeki kendi sayfa tablosuna 8 baytlık yazma ile koymuştur.
* `0x6800000000070f` descriptor'ının fiziksel çıkış-adresi alanı (bits [47:12]) sıfırdır. Yüksek bitler fiziksel adres değildir; descriptor öznitelikleridir.
* Canlı sayfa tablosunu tekrar yürümek de PA `0x0` sonucunu veriyor. Demek ki yalnızca eskimiş TLB girdisi sorunu değil.
* Sonraki `LDR W1, [X8]` fiziksel `0x0` adresinden okumaya çalışıp `Unmapped` alıyor. Ardından kernel `B .` talimatında park ediliyor.

Bu, **anlık abort'un mekanizmasını** kanıtlar. Ancak log tek başına, kernel'in bu PTE'yi niçin oluşturduğunu kanıtlamaz. Bunu çözmek için `boot.img` içindeki tam kernel `Image` ve aynı yapıya ait `vmlinux`/sembol dosyası ya da kernel derleme bilgileri gerekir. `last_abort_pte_writer_pc` tek başına kaynak fonksiyon adını vermez.

## 2. Yeni kod değişiklikleri

### Serial console — tüm oturum günlüğünü tek seferde kopyala

* UI'ya **Tümünü kopyala** düğmesi eklendi.
* Düğme görünür TextView kesitiyle sınırlı değil; ayrı oturum tamponundan kopyalar.
* Görünür konsol geçmişi eski davranışla 24.000 karakterle sınırlıdır; kopyalama tamponu 2.000.000 karakter tutar.
* 2.000.000 karakter sınırı aşılırsa en eski kısım kırpılır ve kopyalanan metnin başına uyarı eklenir.

### Legacy `boot.img` başlık adreslerini raporla

Header v0–v2 içindeki `kernel_addr`, `ramdisk_addr`, `tags_addr` ve (v2 için) `dtb_addr` artık parser sonuçlarına taşınıyor. Logda bunlar, kernel `Image` başlığının `text_offset` değerinden hesaplanan mevcut yükleme adresiyle karşılaştırılır. Henüz otomatik adres değiştirmesi yapılmıyor; adres tahminiyle kernel'in yanlış RAM bölgesine yüklenmesini istemiyoruz.

### Gerçek uyumluluk sınırları görünür olsun

Boot denemesi günlüğü artık MTK probe modunun gerçek MT6768/MT6769 emülasyonu olmadığını açıkça söyler. Ayrıca `super.img` şu anda tek bir ham virtio disk olarak `/dev/block/vda` konumuna bağlanıyor; dinamik `system`, `vendor`, `product`, `system_ext` mantıksal aygıtlarının `super` metadata'sından üretilmesi uygulanmış değildir.

## 3. `tee.img`, `vbmeta.img`, `lk.img` veya başka imaj mı eksik?

Bu logdan “sebep kesinlikle X.img eksik” sonucu çıkarılamaz.

* **`init_boot.img`:** Android 12'de genel ramdisk `boot.img` içinde kalabilir. Android 13 ile çıkan cihazlarda ise `init_boot.img` kullanımı yaygınlaşmıştır. Senin logun header v2 ve gömülü ramdisk/DTB gösteriyorsa sırf Android 12 olduğu için `init_boot.img` zorunlu denemez. Yine de gerçek firmware bölüm düzeni doğrulanmalıdır. Kaynak: AOSP generic boot partition dokümanı — https://source.android.com/docs/core/architecture/partitions/generic-boot
* **`vendor_boot.img`:** Legacy header v0–v2 kullanan yapıda ayrı `vendor_boot` zorunlu olmayabilir. Ancak OEM/vendor ramdisk veya DTB ayrı bir bölüme taşınmışsa matching dosya gerekli olur. Kernel'in vendor modülleri/fstab'ı başka bölümler bekleyebilir.
* **`dtbo.img`:** Telefona özel overlay'ler bu bölümde olabilir. Uygulama mevcut akışta `dtbo.img` seçip DTB overlay uygulamıyor. Firmware'in bölüm listesinde varsa incelenmesi gerekir.
* **`vbmeta.img`:** AVB verified-boot metadatasını taşır. NCV bu akışta bootloader gibi AVB zincirini doğrulamıyor; dolayısıyla mevcut `PA=0` bus abort'unun doğrudan nedeni olduğuna dair kanıt yok. Gerçek Verified Boot davranışı isteniyorsa ayrıca uygulanmalıdır. Kaynak: AOSP Verified Boot — https://source.android.com/docs/security/features/verifiedboot/verified-boot
* **`lk.img`:** Bu uygulama LK bootloader'ını çalıştırmıyor; emülatör direkt EL1'de kernel'e geçiyor. LK dosyasını yalnızca eklemek mevcut sorunu düzeltmez. LK/EL3/secure handoff emülasyonu ayrı ve büyük bir aşamadır.
* **`tee.img`:** TrustZone/secure-world bileşenidir. Daha sonra bazı üretici servisleri için gerekebilir, ancak bu logdaki kernel'in PA `0x0` erişim hatasını doğrudan açıklayan bir kanıt yok.
* **`vendor.img`, `odm.img`, `product.img`, `system_ext.img`:** Android'in ilk aşama init'i ve userspace'i bu bölümlere ihtiyaç duyabilir. Bunların `super.img` içinde bulunması, mevcut NCV motorunun onları otomatik olarak tanıyıp ayrı blok aygıtlarına çevirdiği anlamına gelmez.

## 4. Neden yalnızca `boot.img` + `super.img` şu an yetmiyor?

1. **Donanım modeli uyuşmuyor.** MTK probe seçeneği, tanınmayan MMIO okumalarında sıfır dönme/yazmaları yok sayma şeklinde tanılama kolaylığı sağlar. Bu, gerçek MediaTek saat/reset denetleyicisi, watchdog, PMIC, interrupt yönlendirmesi, IOMMU, Mali GPU veya üretici HAL'larını emüle etmez. Günlükte `W[4] @0x10017040 value=0x0` yok sayılmış bir MTK kayıt yazmasını gösteriyor. Bu erişimin belirli PA=0 eşlemesine neden olduğu kanıtlanmış değil; ancak kernel'in emüle edilmemiş telefon donanımını başlatmaya çalıştığına doğrudan işarettir.
2. **Kernel'e verilen DTB hibrittir.** RAM/GIC/UART/timer/virtio alanları NCV'ye uyarlanıyor; kaynak DTB'deki diğer birçok MediaTek düğümü probe hedefi olarak korunuyor. Bunların var olması, NCV'nin donanımı gerçekleştirdiği anlamına gelmez.
3. **`super.img` henüz dinamik bölüm aygıtlarına açılmıyor.** Android'in dinamik bölüm uygulaması `super` metadata'sını okur ve first-stage init sırasında `dm-linear` aygıtları kurar. NCV şu anda yalnızca ham `super` diskini bir virtio diske bağlıyor. Bu nedenle kernel hatası giderilse bile Android'in `by-name`/logical partition mount adımlarının çalışacağı söylenemez. Kaynak: AOSP dynamic partitions — https://source.android.com/docs/core/ota/dynamic_partitions/implement
4. **Grafik zinciri tamam değil.** NCV'nin basit framebuffer'ı telefonun Mali-G52 + vendor display/HWC/SurfaceFlinger zincirinin yerine geçmez. Kernel bir gün ilerlese bile bu tek başına Android boot animasyonu göstereceği anlamına gelmez.

Infinix'in resmî HOT 20 X6826C ürün teknik belgesi Helio G85 belirtir; cihaz veritabanları bunu MT6769V/CZ olarak eşleştiriyor. Bu nedenle cihazın donanımı genel NCV kartından oldukça farklıdır. Kaynaklar: https://d1wd32b69guoqm.cloudfront.net/infinixmobility_com/HOT20_X6826C_V1.0.pdf ve https://devcheck.app/devices/infinix/infinix-x6826c

## 5. Öncelikli sonraki adım

Bu paketi derleyip aynı `boot.img` ile çalıştır. Önce şu satırları karşılaştır:

```text
[NCV] boot.img header kernel_addr=0x...; ARM64 Image-derived load/entry=0x...
[NCV WARNING] Legacy header kernel_addr differs ...
```

* Adresler aynıysa `kernel_addr` uyuşmazlığı şu aşamada kök neden değildir.
* Adresler farklıysa, doğrudan rastgele yük adresine geçmek yerine imajın boot header'ı, Image `text_offset` ve RAM base arasındaki ilişki doğrulanmalıdır.

Aynı zamanda orijinal firmware bölüm listesinde `dtbo`, `vendor_boot`, `vendor`, `odm`, `vbmeta`, `lk` ve `tee` var mı kontrol edilmelidir. Bunların tümü aynı firmware sürümünden olmalıdır. Fakat bunlar mevcut PTE'nin niçin PA=0 olarak oluşturulduğuna dair eksik kanıtın yerine geçmez.

## Test ve doğrulama durumu

* Release native host testleri: **126/126 geçti**.
* Kotlin/Android APK derlemesi bu çalışma ortamında çalıştırılamadı; Gradle wrapper JAR mevcut değil.
* Gerçek Infinix ROM açılışı doğrulanmadı.

Bu paket, kök nedeni gizlemek için fiziksel `0x0` adresine sahte RAM eşlemesi eklemez. Bu tür bir alias, yanlış MMIO/DTB erişimini gizler ve daha sonra sessiz bellek bozulmasına yol açabilir.
