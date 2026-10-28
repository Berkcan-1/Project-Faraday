# AArch64 sistem davranışı — Stage 4A–4D ilk uygulama dilimleri

**Durum: önemli çekirdek dilimleri uygulanmış durumda; tam ARM64/Android/Linux uyumluluğu değildir. Linux veya Android/GSI boot başarısı henüz kanıtlanmamıştır.**
Güncel host raporu: `docs/test-results/stage4cd-host.md`.

## Uygulanan davranış

- EL0/EL1 state, `SP_EL0`/`SP_EL1`, `SPSel`, DAIF/NZCV, `SVC`, `VBAR_EL1` vektör seçimi, `SPSR_EL1`/`ELR_EL1` ile `ERET` dönüşü.
- Seçili MRS/MSR yazmaçları: `CurrentEL`, `SCTLR_EL1`, `TCR_EL1`, `TTBR0_EL1`, `TTBR1_EL1`, `MAIR_EL1`, `VBAR_EL1`, `ESR_EL1`, `FAR_EL1`, `ELR_EL1`, `SPSR_EL1`, SP ve TPIDR yazmaçlarının bir alt kümesi.
- `SCTLR_EL1.M=1` iken 4 KiB stage-1 translation, 25–48 bit VA aralığı ve 48-bit'e kadar fiziksel adres maskesi.
- 4 KiB granule için dört seviyeli tablo yürüyüşü; level 1/2 block descriptors ve level 3 page descriptors.
- TTBR0/TTBR1 bölgesi seçimi, TCR.TG0/TG1 doğrulaması, TBI0/TBI1'in temel top-byte stripping davranışı, EPD0/EPD1 kontrolü.
- Leaf AF (`Access Flag`), AP (EL0 erişimi ve read-only), PXN/UXN ve tablo descriptor APTable/PXNTable/UXNTable kısıtlarının bir alt kümesi.
- Küçük doğrudan eşlemeli 256 girişli yazılım TLB'si. TTBR0/TTBR1/TCR/SCTLR değişiklikleri ve TLBI benzeri CRn=8 komutları TLB'yi boşaltır.
- MMU etkin olduğunda instruction fetch, scalar load/store, exclusive erişimler ve SIMD/FP yükleme/saklama erişimleri sanal adres çevirisi katmanını kullanır.
- Translation, access-flag, permission, external-bus ve `SCTLR_EL1.A` alignment fault'ları `FAR_EL1`/`ESR_EL1` güncelleyerek EL1 abort vektörüne yönlendirilebilir.

## Bilinen sınırlar / yapılmamışlar

- **Yalnızca 4 KiB granule.** 16 KiB ve 64 KiB granule desteklenmiyor; TCR TG ayarları bu prototipte 4 KiB dışında reddediliyor.
- Tam ARM ARM doğrulaması yapılmadı. TCR invalid configuration, DS/LPA2, 52-bit PA/VA, 16K/64K, hardware AF/dirty update (HA/HD/DBM), contiguous descriptors, stage-2 translation, ASID/global semantics ve tüm abort ISS alanları tam uygulanmadı.
- TLB, en basit tek-vCPU modeli için kasıtlı olarak basittir. İlgili TLBI komutları hedefli invalidasyon yerine tüm yazılım TLB'sini boşaltabilir. Bu uygulama performans/uyumluluk açısından optimize değildir.
- CRn=7 cache maintenance işlemleri birleşik bellek modeli içinde no-op'tur; gerçek instruction/data cache modeli yoktur.
- `LDTR/STTR` erişim seviyesi, ARM `ID_AA64*`/feature-ID yazmaçlarının tam kapsamı, `CPACR_EL1`, debug yazmaçları ve kesme maskelerinin tüm mimari semantiği eksiktir. Timer yazmaçları yalnızca yaygın bir alt kümede modellenmiştir.
- GICv2, IRQ ve FDT yalnızca ilk tek-vCPU diliminde vardır; FIQ/SError, SGI/SMP, tam priority/preemption/security semantiği, PSCI, gerçek blok aygıtı/partition modeli ve Android `boot.img`/`vendor_boot` kurulumu tamamlanmamıştır. WFI/WFE şu an gerçek bekleme/uyandırma yerine no-op ipucudur.
- Android 12–17 veya daha ileri Android sürümlerinin misafir olarak boot edildiği kanıtlanmamıştır.

## 4B host test kapsamı

- VA 0x0 → guest physical mapping ile instruction fetch ve 4 KiB data mapping testi.
- EL0 permission fault ve translation fault sonrası FAR/ESR ile lower-EL data-abort vector testi.
- `TLBI VMALLE1` sonrasında stale translation'ın yenilenmesi.
- `SCTLR_EL1.A` alignment abort testi.
- Eski Aşama 1–3 regression seti.

Önceki 4B kapsamı için eski rapor: `docs/test-results/stage4b-host.md`; güncel doğrulama: `docs/test-results/stage4cd-host.md`.

## Aşama 4C — zamanlayıcı ve kesme teslimi (ilk uygulama)

Kod: `devices/ncv_gicv2.{h,cpp}`, `cpu/ncv_cpu.cpp`.

- Tek vCPU için sınırlı GICv2 distributor + CPU interface MMIO modeli.
- 64 interrupt ID alanı; enable/pending/active ve priority mask; PPI 27 (virtual timer) ve PPI 30 (physical timer) hatları.
- `CNTFRQ_EL0`, `CNTPCT_EL0`, `CNTVCT_EL0`, `CNTP_{TVAL,CTL,CVAL}_EL0` ve `CNTV_{TVAL,CTL,CVAL}_EL0`'ın kullanılan alt kümesi.
- Sayaç host monotonic clock üzerinden 1 MHz sanal frekansta ilerler. Bu gerçek silikonun frekans/performans zamanlaması değildir.
- DAIF.I temizken ve GIC teslim koşulları sağlandığında, CPU komut sınırında IRQ alır ve EL1 vektör tablosundaki doğru IRQ slotuna geçer.
- WFI/WFE bu tek iş parçacıklı yorumlayıcıda zamanlama ipucu olarak ele alınır; host thread'i uyutulmaz.

Sınırlar: GICv2'nin tam register/priority/preemption/security modeli, SGI ve SMP, FIQ/SError, level/edge ayrıntılarının tümü, timer virtualization offset'leri, CNTKCTL trap kuralları ve cycle-accurate zamanlama uygulanmış değildir. Bu yüzden “tam GICv2” değildir; Linux bring-up için gereken en küçük ilk dilimdir.

## Aşama 4D — muhafazakâr CPU özellik profili ve Linux DTB teslimi

Kod: `loader/ncv_dtb.{h,cpp}`, `loader/ncv_linux_image.{h,cpp}`, `Vm::loadLinuxImage`, `Vm::prepareLinuxBootDtb` ve JNI/Kotlin köprüleri.

- ARMv8-A `ID_AA64*` yazmaçlarından yaygın bir alt küme okunabilir; ilan edilmeyen opsiyonel özellikler sıfır raporlanır. `MIDR_EL1` sentetik bir A53-benzeri kimliktir, gerçek host işlemci kimliği değildir.
- `CLIDR_EL1`/`CCSIDR_EL1` cache hiyerarşisini mevcut modelin sınırlı olduğunu belirtecek şekilde raporlar; `DCZID_EL0.DZP` DC ZVA'yı kullanılmaz gösterir.
- FDT v17 biçiminde tek CPU, RAM, PL011, GICv2 ve ARMv8 generic timer düğümleri üretilebilir; `bootargs` ve isteğe bağlı initrd aralığı `/chosen` içine yazılır.
- `Vm::prepareLinuxBootDtb` DTB'yi guest RAM'in üst tarafına yerleştirir, başlangıç EL'ini EL1 yapar ve DTB'nin fiziksel adresini X0'a geçirir.
- Kotlin/JNI `loadLinuxImage` köprüsü, 64-bayt standart başlığı doğrulanabilen **açılmış / sıkıştırılmamış ARM64 `Image`** dosyasını 2 MiB hizalı RAM tabanına göre `text_offset` konumuna yerleştirir; EL1 başlangıç durumunu ve kernel giriş adresini ayarlar. `prepareLinuxBootDtb` daha sonra DTB'yi yerleştirir, X0'a adresini aktarır ve bilinen kernel/initrd/DTB çakışmalarını ve DTB altındaki 64 KiB başlangıç-stack alanına kernel/initrd yerleşimini reddeder.
- `image_size=0` olan eski Image başlıklarında örtüşme kontrolü dosya boyutunu alt sınır kabul eder; gerçek kernel bellek ayak izi bilinmediğinden bu eski biçimde güvenli boşluk garantisi daha zayıftır.

Sınırlar: Bu, Android `boot.img`/`vendor_boot` ayrıştırıcısı değildir; Gzip `Image.gz` açma eklendi; buna karşın Android boot image header sürümleri, `boot.img`/`vendor_boot` ayrıştırma, bootconfig, AVB/vbmeta, dynamic partitions, `super.img`, initramfs/vendor ramdisk ve Android bölüm bağlama henüz yoktur. Üretilen DTB henüz gerçek Linux kernel ile doğrulanmadı; host testi yalnızca Image yerleşimi/başlangıç adresi, DTB yapısı ve X0 handoff'unu doğrular. ID register kapsamı ARM ARM'nin tam uygulaması değildir; gerçek bir Linux kernel'in ihtiyaç duyduğu tüm sistem yazmaçları/komutlar tamamlanmış değildir.
