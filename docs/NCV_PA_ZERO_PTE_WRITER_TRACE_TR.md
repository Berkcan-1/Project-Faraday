# NCV PA=0 PTE Writer Trace — Teknik Rapor

Tarih: 2026-10-27  
Motor: `0.1.9-dev-pa-zero-pte-writer-20261027`

## Tam logdan saptanan durum

Tekrarlanan snapshotlarda aynı erişim başarısız oluyor:

- `FAR/VA`: `0xffffff8008025000`
- `last_abort_insn`: `0xb9400101` (`LDR W1, [X8]`)
- `last_abort_pa`: `0x0`
- `last_abort_bus`: `Unmapped`
- `last_abort_l3_desc`: `0x6800000000070f`
- `last_abort_fresh_walk_ok`: `1`
- `last_abort_fresh_pa`: `0x0`
- `same_pc_slices`: yaklaşık `25998` → `30238`, `PC=0xffffff80080894a8`, `last_insn=0x14000000`

Bu, yalnızca TLB'deki eski çeviriye işaret etmiyor. TLB dışı teşhis yürüyüşü de aynı L3 descriptor'ı okuyor ve PA=0 üretiyor. `last_abort_fresh_bus=Ok`, veri erişiminin başarılı olduğu anlamına gelmez; yalnızca yeni yürüyüşün tamamlandığını belirtir. Son gerçek veri okuması `Bus::read(PA=0)` aşamasında `Unmapped` oluyor.

## Sayfa tablosu çözümlemesi

TCR değeri `0x400035b5593519`; ilgili alanlar `T1SZ=25`, `TG1=2` (4 KiB), `IPS=5` (48-bit output address). Hatalı VA için dizinler:

- L1 index `0x0`, root `0x41ec6000`, descriptor `0x5ffd0003`
- L2 index `0x40`, L2 tablo `0x5ffd0000`, descriptor `0x5ffcf003`
- L3 index `0x25`, L3 tablo `0x5ffcf000`
- L3 giriş fiziksel adresi: `0x5ffcf000 + 0x25 * 8 = 0x5ffcf128`

Descriptor `0x6800000000070f` içindeki bit 54/53/51 gibi yüksek bitler Linux arm64 PTE'sinde UXN/PXN/DBM gibi öznitelik bitleri olabilir; bunlar bu 48-bit biçimde fiziksel OA olarak yorumlanmamalıdır. Fiziksel output-address bitleri `[47:12]` bu descriptor'da sıfırdır. Dolayısıyla bu kayıt gerçekten VA sayfasını PA `0x0`'a eşliyor.

Bu log tek başına, PTE'yi hangi kernel talimatının yazdığını ya da bunun CPU emülasyonunda hatalı bir aritmetik/atomic işlemden mi yoksa guest kernel'in beklenmeyen bellek haritasından mı kaynaklandığını kanıtlamıyor.

## Bu paketteki değişiklik

- `Cpu::storeMemory()` başarılı 64-bit store'lardan, `[47:12]` output-address alanı sıfır olan ve geçerli L3 page descriptor'ı gibi görünen değerleri küçük sabit kapasiteli bir ring buffer'a kaydeder.
- Bir data abort olduğunda TTBR1/L1-L2-L3 zinciriyle tam L3 girişinin fiziksel adresi hesaplanır.
- Kayıtlı store'lar içinde bu tam fiziksel PTE girişine ve aynı descriptor değerine uyan bir yazma aranır.
- Yeni snapshot alanları: `last_abort_l3_entry_pa`, `last_abort_pte_writer_found`, `last_abort_pte_writer_pc`, `last_abort_pte_writer_insn`, `last_abort_pte_writer_va`, `last_abort_pte_writer_pa`, `last_abort_pte_writer_value`, `last_abort_pte_writer_size`.
- Normal guest adres çevirisi/guest bellek davranışı değiştirilmiyor; bu teşhis amaçlıdır.

## Log nasıl yorumlanır?

- `last_abort_pte_writer_found=1`: Ring buffer, tam PTE girişine aynı descriptor değerini yazan 64-bit guest store kaydetmiş. `...writer_pc`, `...writer_insn`, `...writer_va` ve `...writer_pa` bu store için kanıt sağlar. Sonraki adım, store PC'sini ARM64 kernel `vmlinux` sembolleriyle eşlemek ve bu store öncesindeki operandları kontrol etmektir.
- `last_abort_pte_writer_found=0`: Bu, hiçbir store yapılmadığını ispatlamaz. Entry byte/word store'larla hazırlanmış, iz ring'i dolmuş, değer host/loader tarafından konmuş veya yazma gözlem desenine uymamış olabilir.
- Bu patch döngüyü çözen bir düzeltme değil, PTE'yi oluşturan guest talimatını saptamak için teşhis ekidir.

## Testler ve sınırlar

- Release host testleri: 126/126 geçti.
- ASan/UBSan host testleri: 126/126 geçti.
- TSAN bu paket için çalıştırılmadı.
- Android APK build'i ve gerçek cihaz ROM boot'u bu ortamda doğrulanmadı. Projedeki `gradle/wrapper/gradle-wrapper.jar` yokluğu Gradle Wrapper çalıştırılmasını engelleyebilir.
