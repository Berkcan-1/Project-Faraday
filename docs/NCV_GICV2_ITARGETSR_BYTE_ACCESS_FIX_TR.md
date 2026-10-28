# NCV — GICv2 ITARGETSR byte-access fix

Tarih: 27 Ekim 2026  
Engine: `0.1.9-dev-pa-zero-mmu-diag-20261027`

## Logdan belirlenen kök neden

Gönderilen son runtime snapshot içindeki kritik alanlar:

```text
last_abort_va=0xffffff80080058ef
last_abort_pa=0x80008ef
last_abort_size=1
last_abort_access=write
last_abort_bus=InvalidArgument
last_abort_insn=0x39000109
ESR_EL1=0x96000050
DFSC=0x10: sync external abort (not table walk)
```

`last_abort_pa` değerini 7/8 haneli gösterim farkından bağımsız olarak `0x080008ef` şeklinde normalize edersek, NCV GIC Distributor tabanı `0x08000000` için register offset'i `0x8ef` olur. Bu, `GICD_ITARGETSR` byte alanı içinde, interrupt ID 239'un hedef byte'ıdır. `0x39000109`, `STRB W9, [X8]` biçiminde 8 bitlik bir store ile uyumludur.

Önceki `GicV2::validAccess()` yalnızca 4 bayt hizalı erişimleri kabul ediyordu. Dolayısıyla geçerli GICv2 byte-access register'ına gelen 1 baytlık yazı `Status::InvalidArgument` döndürüyor; CPU bunu guest Data Abort / senkron external abort olarak görüyordu. Sonrasında gözlenen `B .` döngüsü kernel'in hata/park yoludur; tek başına tam olarak hangi kernel fonksiyonunda olduğunu kanıtlamaz.

Arm GIC mimari belgesi `GICD_IPRIORITYR`, `GICD_ITARGETSR`, `GICD_CPENDSGIR` ve `GICD_SPENDSGIR` register'ları için byte erişimlerini desteklenen erişim olarak listeler. Diğer GIC register'larını gelişigüzel byte erişimine açmak yerine bu alanlar özel olarak ele alınır.

## Uygulanan değişiklik

- `GicV2` artık 1 baytlık erişimleri yalnızca `IPRIORITYR`, `ITARGETSR`, `CPENDSGIR` ve `SPENDSGIR` aralıklarında kabul eder.
- `IPRIORITYR`/`ITARGETSR` byte okuma ve yazmaları ilgili byte alanına yönlendirilir.
- `CPENDSGIR`/`SPENDSGIR` byte işlemleri, register word'ünün doğru byte lane'ine uygulanır.
- Kontrol register'ları için hizasız word ve desteklenmeyen byte işlemleri hâlâ reddedilir; geniş kapsamlı bir "her MMIO erişimini yoksay" kaçamağı eklenmemiştir.
- Gerçek logdaki fiziksel erişim adresi için regresyon testi eklendi: `Bus` üstünden `0x080008EF`, size `1`.
- Host testinde eski `GICD_TYPER=1` beklentisi güncellendi; bu proje modeli 256 interrupt ID bildiriyor (`ITLinesNumber=7`).
- Eski APK doğrulama script'lerinin motor sürüm işaretçileri mevcut sürüme güncellendi.

## Test ve sınırlar

Release host derlemesi ve testleri **122/122** geçti; ASan+UBSan derlemesi ve testleri de **122/122** geçti. Bunlar MMIO modelindeki regresyonu doğrular; Android APK derlemesi veya gerçek telefon ROM'unun tam açılışı olarak yorumlanmamalıdır.

Bu düzeltmeden sonra guest yeniden başlatıldığında `last_abort_pa=0x080008ef` / `last_abort_size=1` / `last_abort_bus=InvalidArgument` hatasının ortadan kalkması beklenir. Yeni bir abort görülürse yeni `last_abort_pa`, `last_abort_size`, `last_abort_insn`, `last_abort_bus` ve `MTK MMIO probe snapshot` esas alınmalıdır.

Bu emülatör hâlâ tam MT6768 / Helio G85 SoC emülasyonu değildir. Bu yama CPU instruction set'i, fiziksel boot zinciri, eksik clock/reset/PMIC/IOMMU/storage/GPU modellemesi, vendor HAL, AVB veya dynamic partition akışını tamamlamaz; yalnızca bu logdaki GIC erişim hatasını hedefler.
