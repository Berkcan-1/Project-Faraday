# Test raporu: Aşama 2c tamamlandı (2c-2b + 2c-3, HAFİF doğrulama)
Tarih: 2026-10-22 · commit: yok · Host: Linux container (Ubuntu 24.04, x86_64, g++ 13.3.0, cmake 3.28.3, binutils-aarch64-linux-gnu, OpenJDK 21)
**Android cihaz bu turda kullanılmadı; yalnızca host testleri.**

| Kontrol | Sonuç |
|---|---|
| ASAN+UBSAN / TSAN / Release | PASS / PASS / PASS — 62 test, 0 başarısız |
| Yeni oracle seti (HAFİF) | 835 komut şablonu, 1.898 vaka (FP vektör, doyurucu, uzun/geniş/dar, by-element, skaler-SIMD); sonuç + FPSR (bayraklar ve QC) + kaynakların/NZCV'nin bozulmaması |
| Eski testler | Değişmedi, hepsi PASS (2a/2b/2c-1/2c-2a) |
| JNI host testi (gerçek JVM) | PASS |
| Mutasyon bataryası | **BU TURDA YAPILMADI** (hız tercihiyle). 2c-2b/2c-3 için testlerin hata yakalama gücü ölçülmedi. |
| Elle kontrol | FRECPE(1.0f) ve FRSQRTE(1.0f) = 0.998046875: pseudocode'dan elle hesaplandı ve gerçek ARM donanımından hatırlanan değerle uyuştu (donanımda doğrulanmadı) |

## Bu turda testlerin bulduğu hatalar (düzeltildi)
- Skaler dar komutlar (SQXTN b0,h1 vb.): skaler kodlamada Q=1 olduğu için vektör '2' biçimi gibi çalışıyordu.
- Üretici hataları: by-element uzun komutlar için yanlış arrangement (assembler yakaladı).

## Güven düzeyi ve sınırlamalar
- Oracle ve C++ aynı kişi tarafından yazıldı; ARM ARM ile satır satır karşılaştırma yok. Özellikle FRECPE/FRSQRTE tahmin tabloları, FRECPS/FRSQRTS özel durumları, FCVTXN (round-to-odd), FP bayrakları (UFC) ve NaN yayılımı için ortak yanlış anlama riski var.
- FCVTXN için oracle testi YOK (C++ tarafı yalnızca kod olarak mevcut). FRECPX, URECPE/URSQRTE, SDOT/UDOT, FP16 yok.
- Şablon başına 2-3 vaka: nadir kenar durumları (özellikle doyurucu sınırlar, FZ/DN kombinasyonları) örneklenmemiş olabilir.
- FP/SIMD erişimi her zaman açık (CPACR_EL1 Aşama 4'te). Performans ölçülmedi.
