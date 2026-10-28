# NCV GICD Bank-7 Fix (2026-10-27)

## Belirti

Son Android boot denemesi duruyordu:

```text
last_abort_va=0xffffff800800511c
last_abort_pa=0x0800011c
last_abort_size=4
last_abort_access=write
last_abort_bus=DeviceError
last_abort_insn=0xb900010a
```

NCV sanal GIC distributor tabanı `0x08000000` olduğundan `0x0800011c`, distributor `+0x11c` adresidir: `GICD_ISENABLER7`, yani IRQ 224–255 etkinleştirme bankı.

Önceki GIC modeli sadece 64 interrupt ID ayırıyordu (`kInterruptCount=64`, iki banka) ve `GICD_TYPER` için `ITLinesNumber=1` döndürüyordu. `write32()` yalnızca `ISENABLER0/1` (`0x100`, `0x104`) için yazmayı biliyordu. Bu nedenle `0x11c` için `DeviceError` dönüyor ve misafir kernel EL1 Data Abort alıyordu; kernel ardından `B .` bekleme/hata döngüsüne girebiliyordu.

## Değişiklikler

- Tek CPU'lu sanal GICv2 modeli 64'ten 256 interrupt ID'ye genişletildi (8 x 32-bit banka).
- `GICD_TYPER` artık `ITLinesNumber=7` döndürüyor; bu 256 interrupt ID anlamına gelir.
- Enable/disable, pending/clear-pending, active/clear-active, group, priority, target ve configuration kayıtları sekizinci bank dahil destekleniyor.
- SGI pending kayıtları ve tek sanal CPU için temel `GICD_SGIR` davranışı eklendi.
- Map edilmiş GIC penceresindeki hizalı ama emüle edilmeyen/rezerve register erişimleri RAZ/WI davranışıyla dış bus abort üretmiyor. Haritalanmış GIC penceresinin dışındaki adresler yine bus tarafından reddedilir.
- `GICC_APR/NSAPR` ve `DIR` desteği korundu.
- `native-tests/test_gicv2.cpp` eklendi ve host CMake test listesine bağlandı.
- Native motor kimliği `0.1.7-dev-gicd-banks-20261027` olarak güncellendi.

## Doğrulama

Bu pakette odaklı GIC testleri ayrı bir küçük C++ çalıştırıcısıyla denendi:

- Release: 5/5 geçti
- ASan + UBSan: 5/5 geçti
- ThreadSanitizer: 5/5 geçti

Testler `0x08000000` tabanına GIC aygıtı eşleyip logdaki **tam fiziksel adrese `0x0800011c`** yazma/okuma yapar. Ek olarak TYPER, banka-7 pending/active/priority/target/config kayıtları, IRQ 255 teslimi ve rezerve kayıtların RAZ/WI davranışı test edilir.

## Sınırlar

Bu değişiklik önceki `DeviceError` nedenini düzeltir; gerçek Android boot başarısını kanıtlamaz. Model hâlâ tek CPU'ludur ve tam GICv2 güvenlik/virtualization, SMP, tüm interrupt öncelik/koşul kuralları veya MT6768 platformu emülasyonu değildir. Android Studio APK derlemesi ve telefonda yeni boot denemesi bu raporun parçası değildir.
