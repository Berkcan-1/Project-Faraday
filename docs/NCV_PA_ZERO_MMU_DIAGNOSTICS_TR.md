# PA=0 MMU fault diagnostics (27 Ekim 2026)

## Kullanıcı logundaki durum

Gözlenen fault:

```text
last_abort_va=0xffffff8008025000
last_abort_pa=0x0
last_abort_size=4
last_abort_access=read
last_abort_bus=Unmapped
last_abort_insn=0xb9400101
last_abort_table_walk=0
esr_el1=0x96000010
pc=0xffffff80080894a8
last_insn=0x14000000
same_pc_slices=14634
```

`0xb9400101`, `LDR W1, [X8]` ile uyumludur. Guest sanal adresi `0xffffff8008025000` fiziksel `0x0` adresine çevrilmiş; bus'ta RAM/aygıt olmadığı için okuma `Unmapped` olmuştur. `0x96000010`, aynı EL'de bir data abort ve DFSC `0x10` (senkron external abort, page-table-walk dışı) ile uyumludur. Sonrasında `0x14000000` (`B .`) ve büyüyen `same_pc_slices`, kernel'in hata/park döngüsünde kaldığını gösterir.

`W[4] @0x10017040 value=0` ise MTK fallback'in fiziksel vendor bölgesine yapılan yazmayı yalnızca kaydettiğini ve yok saydığını gösterir. Bu yazmanın `PA=0` okumasına doğrudan neden olduğu kanıtlanmış değildir. Rastgele `PA=0` RAM alias'ı eklemek güvenli bir kök neden düzeltmesi değildir; yanlış adres çevirisini maskeleyebilir.

## Bu yamadaki değişiklik

Hata anındaki MMU bağlamı runtime snapshot'a eklendi:

- `last_abort_tcr_el1`, `last_abort_ttbr0_el1`, `last_abort_ttbr1_el1`
- `last_abort_x8`
- `last_abort_walk_root`, `last_abort_walk_va_page`, `last_abort_walk_ttbr1`
- `last_abort_l1_desc`, `last_abort_l2_desc`, `last_abort_l3_desc`
- `last_abort_tlb_hit`

Descriptor alanları ham 64-bit ARM64 translation-table descriptor değerleridir. 4 KiB granule kullanan 39-bit TTBR1 yolu tipik olarak L1/L2/L3 descriptor'ları kullanır. TLB hit durumunda descriptor yolu TLB girdisinden de raporlanır.

## Sonraki log nasıl yorumlanmalı?

- `last_abort_l3_desc` (ya da block mapping varsa L1/L2 descriptor) adres alanında sıfır gösteriyor ve descriptor valid ise, mevcut guest page table bu VA'yı fiziksel sıfıra eşliyor olabilir. `TCR_EL1`/`TTBR1_EL1` ve descriptor zinciri buna göre incelenmelidir.
- `last_abort_tlb_hit=1` çevirinin bu byte erişimi için emülatör TLB girdisinden geldiğini söyler. Bu tek başına TLB hatasını kanıtlamaz; kayıt, ardışık 4-byte erişimin son byte'ında da hit gösterebilir.
- Descriptor zinciri geçersiz veya beklenmedikse, ilk olarak MMU walk/TLBI/cache semantiği ve guest page-table belleği kontrol edilmelidir.
- `last_abort_pa=0` için otomatik düşük RAM alias'ı açılmamalıdır; önce PTE ve TTBR verileri doğrulanmalıdır.

## Doğrulama

- Release host build başarılı.
- Release host testleri: **122 test geçti, 0 başarısız**.
- ASan/UBSan host testleri: **122 test geçti, 0 başarısız**.
- TSAN adımı bu düzenleme oturumunda süre sınırına ulaşmıştır; TSAN geçtiği iddia edilmemektedir.
- Android APK derlemesi ve gerçek telefon ROM'u boot testi burada çalıştırılmadı.

Bu değişiklik root cause'u otomatik düzeltmez; sonraki guest fault'un nedenini belirleyecek ham MMU bağlamını görünür kılar.
