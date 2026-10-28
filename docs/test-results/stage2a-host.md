# Test raporu: Aşama 2a (CPU komut genişletmesi, host)
Tarih: 2026-10-22 · commit: yok · Host: Linux container (Ubuntu 24.04, x86_64, g++ 13.3.0, cmake 3.28.3, binutils-aarch64-linux-gnu)
**Android cihaz bu turda kullanılmadı; yalnızca host testleri.**

| Kontrol | Sonuç |
|---|---|
| ASAN+UBSAN | PASS — 51 test, 0 başarısız |
| TSAN | PASS — 51/51 |
| Release | PASS — 51/51 |
| Oracle vakaları | 440 komut şablonu, 11.674 vaka: x0, NZCV ve kaynak register'ların bozulmaması kontrol edildi |
| Mutasyon kontrolü | 11/11 bilinçli hata yakalandı (32-bit V bayrağı, csneg, sbfm, decodeBitMasks, ldpsw, sdiv INT_MIN/-1, umulh, ldrsw literal, tbz bit5, pre-index writeback, movk W) |
| Önceki testler (bus, uart, vm, jni-host, selftest) | PASS (değişmedi; JNI host testi bu turda yeniden koşulmadı) |

## Kanıt yöntemi
- Kodlama: gerçek assembler (decoder'dan bağımsız).
- Beklenen değer: Python tamsayı matematiği (ör. bayraklar karşılaştırmanın matematiksel anlamından; bitfield takma adları anlamlarından; mantık-immediate'te assembler bitmask'i kodlar, beklenen değer literalin kendisidir).
- Elle yazılmış beklentiler: load/store adresleme modları ve literal yükleme (`ls_modes`, `ls_literal`).

## Sınırlamalar
- Oracle ve CPU kodu aynı kişi (ben) tarafından yazıldı; ARM ARM ile satır satır çapraz doğrulama yapılmadı. Ortak bir yanlış anlama iki tarafta da bulunabilir.
- Bellek sıralama, hizasız erişim hataları, MMU davranışı test edilmedi (modellenmiyor).
- LDP/STP'de ikinci erişim hata verirse ilk saklama yazılmış kalabilir (mimari olarak bu durumda belirsizlik var).
- Performans ölçülmedi. Android cihazda çalıştırma bu turda yapılmadı.
