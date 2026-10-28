# NCV — Misafir Abort / Spin-loop Teşhis Yaması

## Kullanıcının logundaki bulgu

`pc=0xffffff80080894a8`, `last_insn=0x14000000` ve `suspected_spin_loop=1` aynı AArch64 `B .` döngüsünü gösterir. Bu, misafir kernel'in hata/park yoluna girdiğini gösterir; Android host tarafındaki `BLASTBufferQueue` veya `EGL` mesajları tek başına bu döngüyü açıklamaz.

Önceki `ESR_EL1=0x96000050` değeri `EC=0x25` (same-EL Data Abort), `WnR=1` (yazma erişimi) ve `DFSC=0x10` (senkron harici abort; çeviri tablosu yürüyüşü dışında) anlamına gelir. Bu rapor; faulting instruction PC, VA, çeviriden sonra kullanılan PA, erişim boyutu/türü ve host Bus durumunu yakalar.

## Yama içeriği

- `runtimeDiagnostics()` artık `ELR_EL1`, `SPSR_EL1`, `last_abort_pc/insn/va/pa/size/access/bus/table_walk` alanlarını yazar.
- `ESR_EL1` için kısa açıklama eklenir.
- MMU page-table descriptor okumalarında bus hatası, external abort-on-table-walk FSC `0x14 + level` ile raporlanır ve descriptor'ın fiziksel adresi kaydedilir.
- MTK probe, yalnızca fallback penceresinde emülasyon yapmaya devam eder; pencere dışındaki unmapped erişimler yoksayılmaz. Bunun yerine `r`/`w` (rejected) olarak izlenir ve normal `Unmapped` hatası döner. Böylece 0 sayaçlarının dışında kalan reddedilmiş adresler de görülebilir.
- Native engine sürümü `0.1.6-dev-gic-apr-20261027` olarak güncellenir.
- Hızlı odaklı host testi hedefi eklendi.

## Android Studio'da uygulama

1. Önce projeyi yedekle.
2. ZIP içindeki yolları proje köküne kopyala ve ilgili kaynakların üzerine yaz. `local.properties`, Gradle Wrapper ve senin LayoutParams/locale düzeltmelerini koru.
3. Çalıştır:

```bash
./gradlew --stop
rm -rf app/.cxx app/build
./gradlew :app:assembleDebug --rerun-tasks
./tools/verify_ncv_faultdiag_apk.sh app/build/outputs/apk/debug/app-debug.apk
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

4. Aynı `boot.img` + `super.img` ile bir kez çalıştır.
5. `Runtime snapshot` içindeki `last_abort_pc`, `last_abort_insn`, `last_abort_va`, `last_abort_pa`, `last_abort_bus`, `last_abort_table_walk` ve yeni MTK probe raporunu sakla.

## Sonuçların yorumlanması

- `last_abort_bus=Unmapped` + `last_abort_table_walk=0`: misafir veri erişimi çevrilen PA'da hiçbir RAM/aygıta ulaşmadı; `last_abort_pa` hangi aralığın eksik olduğunu gösterecek.
- `last_abort_bus=DeviceError` veya `OutOfBounds`: adres eşlenen bir aygıta ulaştı, ancak aygıt bu erişimi kabul etmedi.
- `last_abort_table_walk=1`: TTBR/page-table descriptor okuması başarısız oldu; `last_abort_pa` descriptor adresidir.
- `last_abort_bus=Ok` ve `last_abort_pa=0`: fiziksel bus hatasından ziyade MMU translation/permission/access-flag fault'u olabilir; ESR decode'a bak.

## Doğrulama

Bu ortamda C++ Release motoru derlendi; odaklı MTK probe testi **5/5 geçti**; JNI C++ dosyası JDK JNI başlıklarıyla `-fsyntax-only` kontrolünden geçti. Android APK derlemesi ve gerçek cihaz/Android boot bu ortamda çalıştırılmadı. Bu paket teşhisi iyileştirir; GSI'nin açıldığını iddia etmez.


## Sonraki bulgu: GICv2 GICC_APR0

Kullanıcıdan gelen gerçek guest logunda `last_abort_pa=0x080100d0`, 4 baytlık yazma ve `last_abort_bus=DeviceError` raporlandı. NCV GIC tabanı `0x08000000`, CPU-interface tabanı `0x08010000` olduğundan bu tam olarak CPU-interface offset `0xD0` olan `GICC_APR0` register'ıdır. Önceki GIC modeli bu register aralığını desteklemediği için `DeviceError` döndürüyordu.

Düzeltme: `GICC_APR0..APR3` (`0xD0..0xDC`), `GICC_NSAPR0..NSAPR3` (`0xE0..0xEC`) için tek sanal CPU durum saklama; `GICC_AIAR/AEOIR/AHPPIR`, opsiyonel status register ve `GICC_DIR` için temel erişim davranışı eklendi. Bu GICv2'nin tam emülasyonu değildir; tek CPU board'un Linux tarafından kullanılan bilinen register'lara erişimini sağlamlaştırır.

Odaklı testler fiziksel `0x080100D0` adresine Bus üzerinden yazıp tekrar okur ve `DeviceError` oluşmadığını doğrular. Bir sonraki boot denemesi yeni bir register'a erişirse yeni loga göre ilerlenmelidir.
