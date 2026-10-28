# Test raporu: çekirdek iskelet (host)
Tarih: 2026-10-21 · commit: yok (git deposu değil)
Host: Linux container, Ubuntu 24.04, x86_64, g++ 13.3.0, cmake 3.28.3, OpenJDK 21, binutils-aarch64-linux-gnu (assembler)
**Hedef cihaz/Android: BU TURDA KULLANILMADI.**

| Kontrol | Komut | Sonuç |
|---|---|---|
| Derleme + testler, ASAN+UBSAN | `cmake -S . -B build -DNCV_SANITIZE=address,undefined && cmake --build build && ./build/ncv_tests` | PASS — 48 test, 0 başarısız, derleme uyarısı yok |
| Thread testleri, TSAN | `-DNCV_SANITIZE=thread` | PASS — 48/48, 0 ThreadSanitizer uyarısı |
| Release | `-DCMAKE_BUILD_TYPE=Release` | PASS — 48/48 |
| Mutasyon kontrolü | `b.ge` mantığı ters çevrildi | testler FAIL verdi (beklenen); değişiklik geri alındı |
| JNI host testi | `libncv.so` (host g++) + Java aynası + OpenJDK 21 | PASS — sürüm, selftest, tam yaşam döngüsü, ölü/bilinmeyen handle, null/aşırı payload, çalışırken destroy |
| Android NDK derlemesi (arm64-v8a) | — | **NOT RUN** |
| APK kurulum/açılış | — | **NOT RUN** |
| Kotlin dosyaları derleme | — | **NOT RUN** |
| Performans | — | **NOT RUN** (iddia yok) |

## Kanıtlananlar
- Seçilen komut alt kümesi, assembler üretimi kodlamalarla beklenen register/bayrak/bellek sonuçlarını veriyor (host).
- Guest payload'ı emüle UART'a `NCV GUEST BOOT OK\n` yazıyor; çıktı Kotlin/JNI sabit metni değil, çalıştırılan guest kodundan geliyor.
- Hatalı erişimler (eşlenmemiş adres, sınır aşımı, uygulanmamış UART register'ı, hizasız/erişilemeyen PC, desteklenmeyen komut) host'u çökertmeden kontrollü durma sebebi üretiyor.
- pause() döndükten sonra CPU ilerlemiyor; stop/reset/destroy-while-running takılmıyor (TSAN temiz).

## Henüz kanıtlanmayanlar
Android'de derlenme/çalışma, release APK, düşük bellekli cihaz davranışı, gerçek ARM64 host'ta çalışma (testler x86_64'te koştu; `unsigned __int128` NDK clang arm64'te desteklenir ama denenmedi), Linux/Android boot, GSI, performans.

## Bilinen sınırlamalar / riskler
- Komut alt kümesi çok küçük (bkz. `compatibility-matrix.md`).
- Testlerin beklenen değerleri (hand-written) ve oracle'ı benim yazdığım mantığa dayanıyor; ARM ARM ile satır satır çapraz doğrulama yapılmadı. Koşul kodları için Python oracle matematiksel tanımdan üretildi.
- UART PL011 TRM ile doğrulanmadı.
