# NCV — Android boot bellek rezervasyonu düzeltmesi

## Bulgular

Son canlı sayfa-tablosu teşhisinde aşağıdaki değerler tekrarlandı:

- Hatalı sanal adres: `0xffffff8008025000`
- Hatalı fiziksel adres: `0x0`
- İşlem: 4 baytlık okuma (`LDR W1, [X8]`)
- Orijinal ve TLB bypass'lı walk aynı L1/L2/L3 descriptor'larını gördü.
- L3 descriptor: `0x006800000000070F`; fiziksel çıktı adresi alanı sıfır olduğundan çeviri fiziksel `0x0` üretiyor.
- Veri erişimi bus katmanında `Unmapped` ile bitiyor. `last_abort_fresh_bus=Ok`, yalnızca fresh page-table walk'un bus hatası olmadan tamamlandığını gösterir; PA=0 okumasının başarılı olduğu anlamına gelmez.

Kök neden henüz kesin kanıtlanmış değil; ancak kaynak kodunda gerçek bir bellek yerleşimi tutarsızlığı saptandı. `prepareAndroidBootDtb()` DTB'nin altındaki 64 KiB aralığı boot-stack/scratch alanı olarak kabul ediyor ve başlangıç `SP`'sini bu aralığa koyuyor. Fakat `buildHybridAndroidDtb()` mevcut rezervasyonları RAM içinde oldukları için siliyor ve bu 64 KiB'yi FDT memory-reservation map'e geri eklemiyordu. Bu durumda Linux, aynı bölgeyi kullanılabilir RAM sanıp erken sayfa tabloları gibi tahsisler için kullanabiliyor.

Verilen logdaki L2 ve L3 sayfa tabloları (`0x5FFE0000` ve `0x5FFDF000`), DTB başlangıcı `0x5FFE2FE8` kabul edildiğinde bu 64 KiB aralığının içindedir. Sayfa tablolarının burada görülmesi tek başına bozulmanın kanıtı değildir, ancak belirtilen stack/scratch aralığının guest OS açısından da rezerve edilmesi gerektiğini güçlü biçimde destekler.

Linux arm64 boot dokümantasyonuna göre, device tree ile kernel'e bildirilen ve rezerve edilmemiş RAM alanları kernel tarafından kullanılabilir kabul edilir ([Linux arm64 boot protocol](https://docs.kernel.org/6.5/arch/arm64/booting.html)). Bu yüzden yalnızca host tarafında bir `safeTop` değişkeni belirlemek, RAM aralığını guest kernel için rezerve etmez.

## Uygulanan değişiklik

1. Android hybrid DTB üretimi, `addrValue - 64 KiB` ile `addrValue` arasındaki boot-stack/scratch aralığını FDT reservation map'e ekliyor.
2. Rezervasyon eklenmesi DTB boyutunu büyüttüğü için DTB adresi ile rezervasyon aralığının sonu sabitlenene kadar kısa bir fixed-point döngüsü kullanılıyor.
3. Initramfs için var olan `safeTop` kontrolü bu son DTB adresinden hesaplanmaya devam ediyor.
4. DTB raporuna `boot-stack-scratch-reserved=BASE+SIZE` kaydı eklendi.
5. Testler hem reservation map girdisini hem de VM hazırlığında rezerve aralığın final DTB başlangıcında bittiğini doğruluyor.

## Doğrulama ve sınırlar

- Release host testleri: **125/125 geçti**.
- ASan/UBSan paketi bu çalışma ortamında derleme süresi sınırına takıldı; bu yama için sanitizer sonucu yok.
- Android APK derlemesi ve gerçek telefon ROM'u açılışı bu ortamda doğrulanmadı.

Bu yama, host ve guest'in bellek rezervasyonu konusundaki tutarsızlığını giderir. `PA=0` hatasının kesin nedeni olduğunu henüz kanıtlamaz. Yeniden çalıştırmada aynı descriptor görülürse sıradaki adım, guest'in `0x5FFDF128` (L3 PTE) konumuna hangi PC/talimat ile yazdığını yakalamak ve kernel'in gerçek bellek haritasını incelemektir.
