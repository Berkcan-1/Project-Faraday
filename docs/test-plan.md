# Test planı
- Host birim testleri (`native-tests/`): bus, uart, cpu, vm (thread'li), loader, selftest. Sabit girdi, rastgelelik yok.
- Çalıştırma: `tools/run_host_tests.sh` (ASAN+UBSAN, TSAN, Release).
- Komut kodlamaları bağımsız assembler'dan: `python3 tools/gen_payloads.py` (aarch64-linux-gnu-as gerekir; üretilen başlıklar repoda).
- Koşul kodları: 15 koşul × 121 değer çifti = 1815 vektör; beklenen sonuçlar Python'da matematiksel tanımdan.
- JNI host testi: `native-tests/jni/` (Java aynası + gerçek JVM). Komutlar `test-results/stage1-3-host.md` içinde.
- Mutasyon kontrolü: bilinçli hata sokulunca testlerin FAIL verdiği doğrulandı (b.ge ters çevirme).
- Cihaz testleri (Android'de `nativeSelfTest()`, pause/resume, arka plana alma) henüz YOK -> NOT RUN.
- Sonraki: sapma (differential) testleri için bağımsız referans (ör. QEMU yalnızca TEST oracle'ı olarak, ürüne girmeden) değerlendirilebilir; lisans/kapsam kararı sana ait.


## Super image metadata parser checks

- [x] Synthetic raw super image with valid geometry/header/table SHA-256 checksums: two logical partitions returned with expected sizes.
- [x] Synthetic Android sparse image with one RAW chunk: same logical partition table returned without expanding the image.
- [x] Synthetic sparse image mixing RAW, FILL and DONT_CARE chunks: metadata remains readable through the logical-address reader.
- [x] Primary metadata damaged but backup metadata intact: parser falls back to the backup copy.
- [ ] Test against real OEM/AOSP super images from multiple Android versions and both raw/sparse encodings.
- [ ] Full Android Gradle/NDK build and on-device SAF file-picker test.
- [ ] Read-only logical partition block view and ext4/EROFS inspection.
