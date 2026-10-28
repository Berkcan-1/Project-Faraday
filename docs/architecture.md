# NCV mimari (taslak + mevcut durum)

## Katmanlar (mevcut durum)
| Modül | Dosya | Durum |
|---|---|---|
| Ortak tipler (`GuestAddr`, `Status`) | `core/ncv_types.h` | var |
| VM nesnesi + durum makinesi + CPU thread'i | `core/ncv_vm.*` | var, host testli |
| AArch64 yorumlayıcı | `cpu/ncv_cpu.*` | **kısmi alt küme**; Linux'un gerektirdiği opcode/sysreg hataları gerçek boot günlükleriyle kapatılmalı |
| EL0/EL1 + sistem yazmaçları | `cpu/ncv_cpu.*` | kısmi; EL2/EL3 ve birçok mimari ayrıntı yok |
| Stage-1 MMU | `cpu/ncv_cpu.*` | 4 KiB granule için kısmi; tam ARM mimarisi değil |
| Guest fiziksel bellek + MMIO bus | `memory/ncv_bus.*` | sınır kontrollü RAM/aygıt eşleme; MTK probe fallback yalnızca tanılama |
| UART + GICv2 + timer | `devices/ncv_uart.*`, `devices/ncv_gicv2.*`, CPU timer yazmaçları | tek vCPU için kısmi |
| RTC | `devices/ncv_pl031_rtc.*` | PL031 uyumlu temel sayaç/eşleşme/IRQ davranışı |
| Depolama ve RNG | `devices/ncv_virtio_blk.*`, `devices/ncv_virtio_rng.*` | virtio-mmio tek kuyruk/sınırlı profil; RNG misafir RAM'ine DMA yapar |
| Grafik yüzeyi | `devices/ncv_framebuffer.*` | 640×480 yazılımsal framebuffer; GPU/HWC değildir |
| Boot imajları/DTB | `loader/*` | ARM64 Image, Android boot kapsayıcıları ve NCV DTB için kısmi; gerçek Linux/Android boot doğrulanmadı |
| `super.img` liblp metadata | `SuperImageMetadata.kt`, `SuperImageAnalyzer.kt` | Ham + Android sparse imajlarda metadata/checksum doğrulama ve mantıksal bölüm listeleme; bölüm cihazı oluşturma henüz yok |
| Öz-test | `selftest/*`, `native-tests/*` | host üzerinde testler mevcut |
| JNI sınırı | `jni/ncv_jni.cpp` | derleyici sözdizimi kontrolü geçti; Android NDK APK build henüz doğrulanmadı |
| Kotlin köprüsü + framebuffer View | `java/com/aerolon/ncv/*.kt` | yazıldı; Android Studio derlemesi bu ortamda doğrulanmadı |

Henüz tam olmayanlar: ARM64 ISA/sistem register kapsamı, tam GIC/PSCI ve SMP, gerçek Linux/Android kernel boot'u, `super.img` içinden mantıksal bölümlerin ayrı guest block device olarak sunulması, AVB/verity akışı, guest giriş aygıtı, virtio-gpu/Mali, Android HWC/gralloc ve vendor HAL davranışı. `super.img` metadata taraması artık mevcut; bu, dinamik bölümlerin mount edilmesi veya Android kullanıcı alanının çalışması anlamına gelmez.

## Tasarım kararları
- Motor Android'den bağımsız C++17; JNI yalnızca `ncv_jni.cpp` içinde. Böylece host'ta sanitizer ile test edilir.
- Hata taşıma: exception yok, `Status`/`StopReason` kodları. JNI'de her giriş `try/catch(...)`.
- JNI handle'ı ham pointer değil kayıt-defteri id'si; ölü handle çökertmez (`-100` döner).
- Güvenlik: tüm guest erişimleri `Bus` içinde sınır kontrollü; toplam RAM için host üst sınırı (varsayılan 1 GiB);
  UART çıktı tamponu sınırlı (64 KiB); dosya yükleme boyut sınırlı.
- `HLT` = NCV test sözleşmesi (VM durur). Gerçek ARM'da HLT bu işi yapmaz; bu mimari davranış değil.
- Hizasız veri erişimi şimdilik RAM'de serbest (gerçek donanımda MMU kapalıyken hizalama hatası olurdu).

## Sanal board (deneysel)
RAM `0x4000_0000` (yapılandırılabilir), PL011 UART `0x0900_0000`, GICv2 `0x0800_0000`, PL031 RTC `0x0901_0000`, virtio-mmio block aygıtları `0x0a00_0000` penceresi, virtio-rng `0x0a00_2000`, simple framebuffer `0x3F00_0000` (640×480×32). GIC IRQ atamaları ve MMIO adresleri `core/ncv_vm.cpp`, aygıt profili `loader/ncv_dtb.cpp` içindedir. Bu, gerçek bir MTK telefonunun board haritası değil, NCV'nin kendi sanal kartıdır.

## BİLİNMİYOR / doğrulanacak
- PL011 register ayrıntıları ARM TRM ile satır satır doğrulanmadı (ID register'ları, reset değerleri, kesmeler).
- Linux kernel'in boot için zorunlu ihtiyaçları (GICv2/v3, arch timer, PSCI, DTB) — kaynaklarla çıkarılmalı.
- Android GSI'nin ihtiyaç duyduğu vendor/HAL/VINTF gereksinimleri — AOSP belgeleriyle çıkarılmalı.
- Windows ARM64 ISO'su: UEFI + ACPI + GIC + depolama/ekran gereksinimleri çok büyük; kapsam dışı sayılmalı, ayrıca araştırılmalı.


## Current virtual-board device inventory (experimental)

The generated NCV board is not an Infinix/MediaTek board. Current generic devices include PL011 UART (IRQ 33), a minimal single-vCPU GICv2, the ARM generic timer model, PL031 RTC (IRQ 34), virtio-mmio block, virtio-rng (IRQ 63, guest-RAM DMA), and a 640x480x32 simple software framebuffer at `0x3F000000`. PSCI support is a limited single-vCPU HVC/SMC subset. These implementations enable bring-up experiments; they are not complete Linux/Android conformance models. The framebuffer is not a Mali GPU or Android HWC/gralloc implementation. MTK-specific clock/reset/PMIC/IOMMU/Mali and vendor HAL behavior remains unimplemented.
