# NCV: Sessiz misafir kernel tanılama düzeltmesi

## Gözlenen durum

Son cihaz logunda önceki `UnsupportedInstruction` satırı yok. CPU thread'i `Running` durumda kalıyor, fakat UART çıktısı üretmiyor. Bu, kernel'in başarılı boot ettiği anlamına gelmez; CPU sonsuz döngüde olabilir, seri konsol seçimi fiziksel MTK komut satırı tarafından ezilmiş olabilir veya kernel şu anda hata vermeden ilerlemeyen bir yerde olabilir.

## Bu güncellemede yapılanlar

- Boot imajından ve vendor boot'tan gelen `console=`, `earlycon=`, `earlyprintk=`, `androidboot.console=`, `kernel.console=` ve `kernel.earlycon=` seçenekleri temizlenir.
- NCV'nin sanal PL011 seri konsol seçenekleri komut satırının sonuna eklenir:
  `console=ttyAMA0 earlycon=pl011,mmio32,0x09000000`
- Vendor boot bootconfig verisinde aynı fiziksel konsol seçenekleri temizlenir; diğer `androidboot.*` anahtarları korunur.
- Kernel thread'inin her 10.000 talimatlık diliminden sonra ırk koşulu oluşturmadan okunabilen atomik bir tanılama görüntüsü yayımlanır: PC, son opcode, emekliye ayrılan talimat sayısı ve dilim sayısı.
- Guest Console ekranı gerçek etkili kernel komut satırını bir kez, sonra yaklaşık her beş saniyede bir runtime snapshot'ı gösterir.
- Testler, fiziksel `console=ttyMT0` ve `earlycon=mtk_uart` seçeneklerinin çıkarıldığını, `androidboot.hardware=mt6768` gibi diğer parametrelerin korunduğunu denetler.

## Cihazda doğrulama

1. Güncel projeyi Android Studio'da aç; `gradle-wrapper.properties`, `local.properties` ve yerel SDK yollarını koru.
2. `Build > Clean Project` ardından `assembleDebug` çalıştır.
3. Uygulamayı kapatıp APK'yi güncelle. Aktif guest çalışırken APK'yi değiştirme.
4. Aynı `boot.img` ve `super.img` ile yeniden dene.
5. Konsolda şunları kontrol et:
   - `Native engine build: 0.1.1-dev-bootdiag-20261027`
   - `Effective kernel command line: ... console=ttyAMA0 earlycon=pl011,mmio32,0x09000000`
   - `Runtime snapshot: state=Running pc=0x... last_insn=0x... retired=... slices=...`

## Snapshot yorumlama

- `retired` ve `slices` artıyorsa CPU yorumlayıcısı talimat yürütmeye devam ediyor. Bu henüz Linux boot başarısı değildir.
- `pc` sürekli aynı aralıkta ve `retired` artıyorsa guest döngüde olabilir.
- `pc` ilerliyor ama `uart_tx` görünmüyorsa (şu an ayrı sayaç yok) konsol/driver/bootargs uyumluluğu hâlâ şüphelidir.
- `state=Faulted` görünürse hemen arkasındaki `stopInfo` satırındaki `pc`, `insn` ve `reason` sonraki gerçek CPU hatasıdır.
- Snapshot ilerlemiyor veya `pc` sıfırsa yeni `Native engine build` satırıyla birlikte raporlanmalı; eski `libncv.so` hâlâ çalışıyor olabilir.

## APK `base.apk` I/O logu

`SystemUI` logundaki `Failed to open APK ... base.apk: I/O error`, misafir CPU hatasının doğrudan açıklaması değildir; Android host sisteminin uygulama kaynaklarını/ikonunu yükleyemediğini gösterir. APK güncellenirken NCV açık bırakıldıysa uygulamayı kapatıp yeniden `adb install -r` yapmak denenebilir. Tekrarlanırsa `adb shell pm path com.aerolon.ncv` ve kurulum/storage durumunu kontrol et.

## Doğrulama sınırı

Host C++ testleri ve sanitizer testleri çalıştırıldı. Bu çalışma ortamında Android SDK/NDK ile APK derlemesi ve fiziksel telefonda yeni sürüm testi yapılmadı. UART düzeltmesi, PL011'ın vendor kernel'e kesin entegre olduğunu veya Android'in boot edeceğini garanti etmez; yalnızca fiziksel cihaz komut satırının NCV konsol seçimini ezmesini önler ve sonraki çalıştırmayı gözlemlenebilir kılar.
