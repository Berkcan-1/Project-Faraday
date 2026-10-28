# Test raporu: Aşama 2b (tam tamsayı ISA, host)
Tarih: 2026-10-22 · commit: yok · Host: Linux container (Ubuntu 24.04, x86_64, g++ 13.3.0, cmake 3.28.3, binutils-aarch64-linux-gnu, OpenJDK 21)
**Android cihaz bu turda kullanılmadı; yalnızca host testleri.**

| Kontrol | Sonuç |
|---|---|
| ASAN+UBSAN | PASS — 56 test, 0 başarısız |
| TSAN | PASS — 56/56 |
| Release | PASS — 56/56 |
| Oracle vakaları | 513 komut şablonu, 15.613 vaka (x0, NZCV, kaynak register bozulmaması) |
| Elle yazılı programlar | exclusive/acq-rel (başarı, clrex, monitor temizleme, çift), hizasız fault, ldnp/stnp, ldtr/sttr, prfm (eşlenmemiş adreste bile fault yok), hint/bariyer |
| Mutasyon | 12/12 yakalandı. Not: ilk denemede 1 mutasyon kaçtı (başarılı stxr sonrası monitörün temizlenmesi); test eklendi, yeniden denemede yakalandı |
| JNI host testi (gerçek JVM) | PASS (motor değişikliği sonrası yeniden koşuldu) |

## Sınırlamalar
- Oracle ve CPU aynı kişi (ben) tarafından yazıldı; ARM ARM ile satır satır çapraz doğrulama yok.
- Exclusive monitor tek çekirdek içindir; çok çekirdekli/global monitor, bellek sıralaması modellenmedi.
- LDTR/STTR EL0 yetkisi, hizasız normal erişim hatası, MMU davranışı modellenmedi (Aşama 4).
- FP/SIMD yok -> gerçek Android userspace henüz çalışamaz. Performans ölçülmedi. Android cihazda bu tur çalıştırma yapılmadı.
