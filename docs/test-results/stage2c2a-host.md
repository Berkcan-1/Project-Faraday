# Test raporu: Aşama 2c-2a (ASIMD tamsayı alt kümesi + yapı yükleme/saklama, host)
Tarih: 2026-10-22 · commit: yok · Host: Linux container (Ubuntu 24.04, x86_64, g++ 13.3.0, cmake 3.28.3, binutils-aarch64-linux-gnu, OpenJDK 21)
**Android cihaz bu turda kullanılmadı; yalnızca host testleri.**

| Kontrol | Sonuç |
|---|---|
| ASAN+UBSAN | PASS — 61 test, 0 başarısız |
| TSAN | PASS — 61/61 |
| Release | PASS — 61/61 |
| ASIMD register vakaları | 5.802 vaka (lane tabanlı Python oracle; sonuç, kaynakların bozulmaması, NZCV/FPSR değişmemesi) |
| Yapı yükleme/saklama | 1.240 bellek vakası: LD1..LD4/ST1..ST4 (çoklu, tek-lane, replicate, post-index imm/reg); registerlar, bellek ve taban register'ı kontrol edildi |
| Mutasyon (doğrulanmış yeniden derleme, ikili hash değişimi) | 38/38 yakalandı |
| JNI host testi (gerçek JVM) | PASS |

## Bu turda testlerin bulduğu gerçek hatalar (düzeltildi)
- Yapı komutlarında eleman boyutu alanı bit 11:10'da; kod bit 23:22'yi okuyordu (post-index formları reddediliyor, no-offset yanlış sonuç veriyordu).
- Tek-lane yapı komutlarında opcode bit 15:13 ve S bit 12; kod 4 bit okuyup S'yi opcode'a karıştırıyordu.
- TBX maskesi `op` bitini sabit sayıyordu (TBX hiç eşleşmiyordu).
- RBIT/NOT: boyut alanı eleman genişliği değil işlem seçici; yalnızca yarı bayt işleniyordu.
- UBSAN: SRI/SHLL'de tanımsız kaydırmalar (shift=64, negatif sola kaydırma).

## Sınırlamalar
- Oracle (Python) ve C++ aynı kişi tarafından yazıldı; ARM ARM ile satır satır çapraz doğrulama yok. Kodlamalar assembler'dan olduğu için kodlama hataları yakalanır; anlam (semantik) yanlış anlamaları iki tarafta ortak olabilir.
- Doyurucu/yuvarlamalı, uzun/geniş/dar aritmetik, by-element, skaler-SIMD, PMULL, kripto, FP vektörleri YOK (2c-2b, 2c-3) -> Android userspace henüz tam çalışmaz.
- FP/SIMD erişimi her zaman açık (CPACR_EL1 Aşama 4'te). Performans ölçülmedi. Android cihazda bu tur çalıştırma yapılmadı.
