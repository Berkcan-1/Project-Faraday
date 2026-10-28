# Aşama 4B — host doğrulama raporu

Tarih: 2026-10-26  
Kaynak: kullanıcı tarafından sağlanan `ncv_core.zip` ve önceki Stage 4A çalışması üzerine güncelleme.  
Hedef ortam: Linux x86_64 host testleri. Android cihaz/NDK/APK testi yapılmadı.

## Uygulanan kapsam

- `SCTLR_EL1.M` artık zorla sıfırlanmıyor; M biti set ise guest veri erişimleri ve instruction fetch stage-1 MMU yoluna giriyor.
- 4 KiB granule, 25–48 bit VA aralığı, 48-bit'e kadar fiziksel adres alanı ve 4-level translation-table walk.
- Level 1/2 block descriptor ve level 3 page descriptor desteği.
- TTBR0 ve TTBR1 adres bölgesi seçimi; TCR.TG0/TG1, T0SZ/T1SZ, EPD0/EPD1 ve temel TBI0/TBI1 işlemleri.
- AF, AP, PXN/UXN ile sınırlı table descriptor APTable/PXNTable/UXNTable izin kontrolleri.
- 256-entry doğrudan eşlemeli yazılım TLB'si. SCTLR/TCR/TTBR değişiklikleri ve CRn=8 TLBI sınıfı işlemler TLB'yi temizler.
- MMU aktifken scalar, exclusive, SIMD ve FP bellek işlemleri çeviri katmanına yönlendirilir; çeviri tek bir sayfayı aşıyorsa byte-bazlı translation/fault kontrolü yapılır.
- Translation/access-flag/permission/external abort'ları ve SCTLR_EL1.A hizalama fault'ları FAR_EL1/ESR_EL1 üzerinden exception vektörüne yönlendirilir.
- CRn=7 cache-maintenance işlemleri birleşik ve cache modeline sahip olmayan host bellek modeli içinde no-op olarak ele alınır.

## Test kanıtı

`bash tools/run_host_tests.sh` komutu üç ayrı host yapılandırmasını derleyip test etti:

| Yapılandırma | Sonuç |
|---|---:|
| ASan + UBSan | **78 test, 0 başarısız** |
| ThreadSanitizer | **78 test, 0 başarısız** |
| Release | **78 test, 0 başarısız** |

Yeni MMU/abort testleri:

- `arch_mmu_4k_level2_block_descriptor`
- `arch_mmu_ttbr1_high_kernel_virtual_address`
- `arch_mmu_4k_translates_fetch_load_and_store`
- `arch_mmu_4k_permission_fault_vectors_to_el1`
- `arch_mmu_tlbi_vmalle1_invalidates_stale_guest_translation`
- `arch_mmu_access_flag_fault_reports_level_and_far`
- `arch_mmu_uxn_blocks_el0_instruction_fetch_but_allows_el1_vector`
- `arch_mmu_4k_translation_fault_reports_far_esr_and_vectors`
- `arch_mmu_misaligned_pc_takes_instruction_abort`
- `arch_sctlr_alignment_check_takes_data_abort`

Eski CPU, FP/SIMD, bus, loader, VM-state ve Aşama 4A regresyonlarının tamamı da aynı test koşusunda geçti.

## Önemli sınırlar

Bu, **tam ARMv8/AArch64 MMU uygulaması değildir**. Aşağıdaki destekler eksiktir veya yalnızca sınırlı prototiptir:

- 16 KiB/64 KiB granule, 52-bit PA/LPA2, stage-2 translation, hardware AF/dirty-bit update, DBM/HA/HD, contiguous descriptors ve tam ASID/global semantiği.
- Tam ARM ARM doğrulaması ve bağımsız donanım referans testi.
- Tüm feature ID/system register'ları (özellikle `ID_AA64*`, `CTR_EL0`, `DCZID_EL0`), CPACR_EL1 ve eksik komut kapsamı.
- GIC, architected timer, IRQ/FIQ/SError delivery, doğru WFI/WFE, PSCI, DTB üretimi, disk/partition ve Android boot zinciri.
- Gerçek Android/Linux kernel boot, gerçek GSI boot ve Android 12–17/sonraki sürümlerdeki uyumluluk **test edilmedi**.
- Android SDK/NDK bu çalışma ortamında tanımlı değildi; NDK derlemesi, APK üretimi/kurulumu ve telefonda çalıştırma yapılmadı.

**Sonuç:** Stage 4B host testlerinden geçti ve MMU temel yolunu ekliyor. Proje henüz GSI çalıştıracak seviyede değildir. Sıradaki kritik adımlar interrupt/timer/GIC, Android kernel'in ihtiyaç duyduğu system register/feature beyanları, PSCI/DTB ve boot/storage aygıt katmanlarıdır.
