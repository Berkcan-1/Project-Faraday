# Test raporu: Aşama 2c-1 (skaler FP + SIMD&FP bellek, host)
Tarih: 2026-10-22 · commit: yok · Host: Linux container (Ubuntu 24.04, x86_64, g++ 13.3.0, cmake 3.28.3, binutils-aarch64-linux-gnu, OpenJDK 21)
**Android cihaz bu turda kullanılmadı; yalnızca host testleri.**

| Kontrol | Sonuç |
|---|---|
| ASAN+UBSAN | PASS — 59 test, 0 başarısız |
| TSAN | PASS — 59/59 |
| Release | PASS — 59/59 |
| FP oracle vakaları | 218 komut şablonu, 13.190 vaka (sonuç bitleri, FPSR bayrakları, NZCV, kaynakların bozulmaması, üst yarının sıfırlanması); 8 FPCR yapılandırması (RN/RP/RM/RZ, FZ, DN) döngüsel |
| Oracle'ın kendi doğrulaması | `tools/fpo.py selfcheck`: CPython yerel IEEE float/double ile add/sub/mul/div/sqrt ve d->s/h dönüşümlerinde bit bit eşleşti (RN) |
| Elle yazılı | SIMD&FP yükleme/saklama modları (B/H/S/D/Q, unsigned/unscaled/pre/post/register/literal, LDP/STP/LDNP/STNP), mrs/msr (FPCR/FPSR/NZCV/TPIDR_EL0) |
| Mutasyon (doğrulanmış yeniden derleme, ikili hash değişimi) | 29/30 yakalandı. İlk bataryada ardışık hızlı dosya değişimi yüzünden bazı sonuçlar bayat ikiliden gelmiş olabilirdi; batarya mtime ve ikili hash kontrolüyle baştan koşuldu. Kaçan: `mul` içinde `L==127` sticky biti — denk mutant (çarpımın alt 22 biti half/single/double için hep sıfır, kod yoluna ulaşan girdi yok) |
| JNI host testi (gerçek JVM) | PASS |

## Sınırlamalar
- Oracle (Python) ve soft-float (C++) aynı kişi tarafından, ARM ARM'daki FPRound/FPProcessNaNs davranışının hatırlanan hâliyle yazıldı; kılavuzla satır satır karşılaştırma YOK. Ortak bir yanlış anlama iki tarafta da bulunabilir (özellikle NaN yayılımı, UFC tininess kuralı, FMAXNM, FZ kenar durumları).
- Half aritmetiği (FEAT_FP16), AHP, FPCR tuzak bitleri yok. ASIMD vektör komutları ve yapı yükleme/saklama (LD1..LD4) yok -> gerçek Android userspace henüz çalışamaz.
- FP/SIMD erişimi her zaman açık (CPACR_EL1 modeli Aşama 4'te). Performans ölçülmedi. Android cihazda bu tur çalıştırma yapılmadı.
