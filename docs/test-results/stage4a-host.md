# Aşama 4A — host doğrulama raporu

Tarih: 2026-10-26  
Kaynak: kullanıcı tarafından sağlanan `ncv_core.zip` üzerine değişiklik  
Hedef ortam: Linux x86_64 host testleri; Android cihazı/NDK kullanılmadı.

## Uygulanan kapsam

- EL0/EL1 için `current_el`, `SPSel`, `DAIF` ve NZCV durum alanları.
- `SP_EL0` ve `SP_EL1` bankaları; geriye uyum için `CpuState::sp` aktif SP’nin bir aynası olarak tutulur.
- `SVC #imm16` synchronous exception girişi: `SPSR_EL1`, `ELR_EL1`, `ESR_EL1`, `VBAR_EL1` vektör seçimi, EL1h ve maskelenmiş DAIF durumu.
- `ERET`: EL0t, EL1t ve EL1h dönüş modları; NZCV/DAIF ve stack-bank dönüşü. Geçersiz dönüş modu bilinmeyen/illegal exception olarak yakalanır.
- `MSR SPSel, #imm`.
- Seçili system registers için MRS/MSR: CurrentEL, SCTLR_EL1 (M biti daima 0), TCR_EL1, TTBR0/1_EL1, MAIR_EL1, VBAR_EL1, ESR_EL1 (salt-okunur), FAR_EL1, ELR_EL1, SPSR_EL1, SP_EL0, TPIDR_EL1; önceki NZCV/FPCR/FPSR/TPIDR_EL0/TPIDRRO_EL0 erişimleri korunur.
- EL0’den tanınan EL1 yazmaçlarına erişim bir undefined-instruction exception ile EL1’e yönlendirilir. Bilinmeyen yazmaç kodlamaları henüz host `UnsupportedInstruction` durumunda kalır.

## Gerçekleştirilen testler

Her biri temiz, ayrı build dizininde yapılandırıldı, derlendi ve `ncv_tests` çalıştırıldı:

| Yapılandırma | Derleme | Test sonucu | Uyarı/hata |
|---|---|---|---|
| ASan + UBSan (`-DNCV_SANITIZE=address,undefined`) | PASS | **68 test, 0 başarısız** | Derleme uyarısı/hatası yok |
| TSan (`-DNCV_SANITIZE=thread`) | PASS | **68 test, 0 başarısız** | Derleme uyarısı/hatası yok |
| Release (`-DCMAKE_BUILD_TYPE=Release`) | PASS | **68 test, 0 başarısız** | Derleme uyarısı/hatası yok |

Yeni mimari testleri: `arch_current_el_mrs_reports_el0_and_el1`, `arch_svc_enters_el1_and_eret_returns_to_el0`, `arch_el0_privileged_sysreg_access_takes_undefined_exception`, `arch_spsel_switches_banked_stack_pointer`, `arch_svc_from_el1_uses_current_el_spx_vector`, `arch_vm_can_configure_initial_el1`.

Host JVM JNI kontrolü ayrıca tekrar çalıştırıldı: C++ native library derlendi, Java aynası derlendi, yaşam döngüsü testleri ve yeni `nativeCreateVmWithInitialEL` girişi geçti (`JNI HOST TEST: PASS`). `NativeBridge.kt` ve `NcvVm.kt`, `kotlinc` ile ayrıca derlendi (PASS). Bununla birlikte Android Gradle projesi/NDK toolchain bu arşivde yok; Android NDK/APK kurulum testi yapılmadı.

## Henüz yapılmayanlar — Aşama 4 tamamlandı sayılmaz

- MMU çeviri yolu yok: sayfa tablosu walk, 4 KiB/48-bit 4 seviye, AP/AF/UXN/PXN, TLB/TLBI henüz uygulanmadı. SCTLR_EL1.M zorla 0 tutulur.
- Komut getirme ve veri erişim BusFault/FetchFault/AlignmentFault’ları mimari instruction/data abort exception’larına çevrilmiyor; FAR_EL1 bütün fault’lar için geçerli değil.
- IRQ/FIQ/SError, sanal interrupt controller (GIC), architected timer, WFI/WFE bekleme semantiği, PSCI, DTB ve SMP yok.
- Feature ID register’ları (ID_AA64*), CTR_EL0, DCZID_EL0, CNT* timer registers, CPACR_EL1 trap semantiği tamamlanmadı.
- ARM ARM ile tam doğrulama ve bağımsız referans model testi yapılmadı.
- Android NDK/arm64-v8a derlemesi, APK kurulumu ve gerçek cihaz testi **yapılmadı**.
- Linux kernel, Android boot ve GSI **destekleniyor olarak iddia edilemez**.

Bu rapor yalnızca host üzerinde test edilen Aşama 4A kodunu tanımlar.
