# NCV PA=0 / Live Page-Walk Diagnostics (27 Ekim 2026)

## Neden bu sürüm var?

Önceki logdaki `0x` önekli bazı MMU değerleri yanlışlıkla ondalık biçimde yazılıyordu. Nedeni, `runtimeDiagnostics()` içinde `last_abort_size` için `std::dec` kullanıldıktan sonra MMU alanları yazdırılırken `std::hex` durumuna dönülmemesiydi. Bu sürüm bu biçimlendirme hatasını düzeltir.

Ayrıca ARM64 misafir erişimi bir yazılımsal TLB girdisi kullandıktan sonra fiziksel bus erişimi başarısız olursa, yalnızca teşhis amacıyla aynı sanal adresi TLB girdisini geçici olarak bypass ederek yeniden çevirir. Ardından özgün TLB girdisini ve CPU'nun aktif çeviri tanılama durumunu geri yükler; misafir komutunun sonucu/çevirisi değiştirilmez.

## Yeni alanlar

```text
last_abort_fresh_walk_attempted=1
last_abort_fresh_walk_ok=1
last_abort_fresh_pa=0x...
last_abort_fresh_root=0x...
last_abort_fresh_l1_desc=0x...
last_abort_fresh_l2_desc=0x...
last_abort_fresh_l3_desc=0x...
last_abort_fresh_fsc=0x...
last_abort_fresh_table_walk=0|1
last_abort_fresh_fault_pa=0x...
last_abort_fresh_bus=...
```

`last_abort_fresh_*`, mevcut konuk sayfa tabloları yeniden okunduğunda bulunan sonucu gösterir. Bunlar yalnızca hata anındaki teşhis içindir.

## Yorumlama

- `last_abort_fresh_walk_attempted=1` ve `last_abort_fresh_walk_ok=1`, aynı VA için canlı PTE zincirinin yeniden okunabildiğini gösterir.
- `last_abort_pa=0x0`, `last_abort_fresh_pa=0x0` ve canlı L3 descriptor'ın adres alanı sıfırsa, güncel konuk sayfa tablosu bu VA'yı fiziksel sıfıra eşliyor. Bu durumda yalnızca TLB'yi suçlamak doğru değildir; konuk PTE'lerinin nasıl oluştuğu ve boot memory map/DTB yerleşimi incelenmelidir.
- `last_abort_pa=0x0` iken `last_abort_fresh_pa` farklı ve RAM içinde geçerli bir adrese eşitse, eski/cached translation veya emülatörün TLB kullanım yolu şüphelidir.
- `last_abort_fresh_walk_ok=0` ise `last_abort_fresh_fsc`, `last_abort_fresh_table_walk`, `last_abort_fresh_fault_pa` ve `last_abort_fresh_bus` alanları canlı walk'ın nerede başarısız olduğunu göstermek için vardır.
- `last_abort_tlb_hit=1` tek başına önceden kalan bir TLB eşlemesini kanıtlamaz. Bu motor 4 baytlık yüklemeyi byte başına çeviriyor; ilk byte girdiyi oluşturabilir ve sonraki baytlar aynı LDR içinde cache hit olabilir.

## Koruyucu sınırlar

Fiziksel `0x0` adresine RAM alias'ı eklenmedi. Böyle bir alias hatalı PTE'yi maskeleyebilir ve kernel'in yanlış belleği okumasını sağlayabilir. Doğrudan cihaz ROM'unun başarıyla boot edilmesi henüz doğrulanmış değildir.

## Doğrulama

- Release host testleri: 123 test, 0 başarısız.
- ASan/UBSan host testleri: 123 test, 0 başarısız.
- TSAN koşusu bu oturumda süre sınırına ulaştı; başarılı olduğu iddia edilmiyor.
- Android APK ve gerçek ROM boot testi burada doğrulanmadı.
