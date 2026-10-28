# Cihaz katmanı planı — Cuttlefish benzeri kapsam (kısmen uygulanmış; Android boot henüz doğrulanmadı)
Hedef: kernel + ramdisk + `super.img` (ve recovery) ile Android'in açılabildiği, QEMU/Cuttlefish benzeri bir sanal cihaz.

## Dürüst çerçeve
- Cuttlefish, ana makinede donanım sanallaştırmasıyla (KVM) çalışır. NCV ise yazılım emülasyonudur (yorumlayıcı); aynı iş yükü kat kat yavaş olur. Android'in tam boot'u milyarlarca komuttur: yorumlayıcıyla dakikalar-saatler sürebilir. Kullanılabilir hız için ileride ikili çeviri (DBT/JIT) gerekir; bu ayrı ve büyük bir araştırma işidir, şimdilik YOK.
- Aşağıda bir kısmı host üzerinde modellenmiş olsa da hiçbir madde gerçek Android/GSI boot kanıtı sayılmaz. Tamamlanma için host testine ek olarak gerçek kernel/cihaz günlüğü gerekir.

## Bileşenler (önerilen sıra, Aşama 4 sonrası)
1. **Sistem çekirdeği (Aşama 4):** EL0/EL1, kısmi istisna/MMU/sistem register desteği, tek-vCPU GICv2, arch timer ve sınırlı PSCI mevcut; gerçek Linux boot ile eksikler tamamlanmalı. (`docs/stage4-design.md`)
2. **virtio-mmio altyapısı:** sınırlı tek-kuyruk virtio-blk ve virtio-rng, PL011 UART, PL031 RTC ve basit framebuffer mevcut. virtio-console, virtio-net, virtio-input ve virtio-gpu henüz yok; mevcut framebuffer GPU/HWC yerine geçmez.
3. **Disk modeli:** raw/Android sparse imajlar için sınırlı salt-okunur blok yolu var. `super.img` LP/dynamic-partition metadata çözümlemesi ve yazılabilir **overlay (copy-on-write)** henüz tam değil; orijinal imajlar değiştirilmemeli. Cuttlefish'e benzer şekilde, guest'in `/dev/block/by-name/*` görmesi için sanal GPT'li birleşik disk (boot, vendor_boot, init_boot, super, userdata, vbmeta...). Karar gerekiyor: tek birleşik virtio-blk mı, bölüm başına ayrı mı.
4. **Boot akışı:** önce **doğrudan kernel boot** (kernel Image + initramfs/boot.img v3/v4 + vendor_boot + DTB üretimi + kernel cmdline/bootconfig). Bootloader (U-Boot/UEFI) sonra. **Recovery:** boot/recovery ramdisk'iyle aynı mekanizma; recovery UI için framebuffer + giriş gerekir.
5. **AVB / vbmeta:** ilk hedefte doğrulama kapalı imajlarla (verity devre dışı vbmeta); tam AVB sonraya.
6. **Ana makine entegrasyonu (Android uygulaması):** dosya seçici, boot profili, UART konsolu ve misafir software-framebuffer görüntüleme kodu var; dokunma/klavye, ses, GPU/HWC, APK derlemesi ve fiziksel cihaz doğrulaması henüz tamam değil.
7. **Hata ayıklama/gözlem:** konsol günlüğü, komut izi (trace), anlık görüntü (snapshot) — özellikle bring-up için.

## Açık kararlar / BİLİNMİYOR
- Hedef Android sürümü ve ilgili kernel (GKI) sürümü; hangi özellikleri (PAC/BTI/MTE, LSE, SVE) kullandığı. Kullanılan özellik ID register'larında ilan edilmeli ya da kernel o özellikler kapalı derlenmeli.
- GIC v2 mi v3 mü; SMP (çok çekirdek) ilk hedef mi? (öneri: önce tek çekirdek)
- Windows ARM64 ISO: UEFI + ACPI + çok daha geniş donanım gerektirir; Android'den sonraya bırakılmalı ve ayrıca araştırılmalı.
- Performans bütçesi: kabul edilebilir boot süresi ve JIT/DBT'ye geçiş zamanı.
