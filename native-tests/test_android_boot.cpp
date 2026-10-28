#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include "loader/ncv_android_boot_image.h"
#include "loader/ncv_dtb.h"
#include "core/ncv_vm.h"
#include "test_framework.h"

using namespace ncv;
namespace {
void putLe32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
  b[o] = static_cast<uint8_t>(v); b[o + 1] = static_cast<uint8_t>(v >> 8);
  b[o + 2] = static_cast<uint8_t>(v >> 16); b[o + 3] = static_cast<uint8_t>(v >> 24);
}
void putLe64(std::vector<uint8_t>& b, size_t o, uint64_t v) {
  putLe32(b, o, static_cast<uint32_t>(v)); putLe32(b, o + 4, static_cast<uint32_t>(v >> 32));
}
uint32_t getBe32(const std::vector<uint8_t>& b, size_t o) {
  return (static_cast<uint32_t>(b[o]) << 24) | (static_cast<uint32_t>(b[o + 1]) << 16) |
         (static_cast<uint32_t>(b[o + 2]) << 8) | static_cast<uint32_t>(b[o + 3]);
}
uint64_t getBe64(const std::vector<uint8_t>& b, size_t o) {
  return (static_cast<uint64_t>(getBe32(b, o)) << 32) | getBe32(b, o + 4);
}
bool fdtHasReserve(const std::vector<uint8_t>& fdt, uint64_t address, uint64_t size) {
  if (fdt.size() < 40 || getBe32(fdt, 0) != 0xD00DFEEDu) return false;
  const uint32_t total = getBe32(fdt, 4), off = getBe32(fdt, 16);
  if (total > fdt.size() || off > total) return false;
  for (uint64_t p = off; p + 16 <= total; p += 16) {
    const uint64_t a = getBe64(fdt, static_cast<size_t>(p));
    const uint64_t n = getBe64(fdt, static_cast<size_t>(p + 8));
    if (a == 0 && n == 0) break;
    if (a == address && n == size) return true;
  }
  return false;
}
void putMagic(std::vector<uint8_t>& b, size_t o, const char* v, size_t n) {
  std::memcpy(b.data() + o, v, n);
}

std::vector<uint8_t> makeBootV2WithDtb(const std::vector<uint8_t>& dtb) {
  constexpr uint32_t page = 4096, kernelSize = 64, ramdiskSize = 16;
  constexpr uint32_t kernelOffset = page, ramdiskOffset = 2 * page, dtbOffset = 3 * page;
  std::vector<uint8_t> b(static_cast<size_t>(dtbOffset) + dtb.size(), 0);
  putMagic(b, 0, "ANDROID!", 8);
  putLe32(b, 8, kernelSize); putLe32(b, 12, 0x40080000u);
  putLe32(b, 16, ramdiskSize); putLe32(b, 20, 0x44000000u);
  putLe32(b, 32, 0x40000100u);
  putLe32(b, 36, page); putLe32(b, 40, 2); putLe32(b, 1644, 1660);
  putLe32(b, 1648, static_cast<uint32_t>(dtb.size())); putLe64(b, 1652, 0x41000000ull);
  putMagic(b, kernelOffset, "NCV-KERNEL-TEST", 15);
  // Minimal ARM64 Image header so VM integration tests can exercise the boot path.
  putLe64(b, kernelOffset + 0x08, 0x80000ull);  // text_offset
  putLe64(b, kernelOffset + 0x10, 0);           // legacy unknown image_size
  putLe64(b, kernelOffset + 0x18, 0);           // little-endian, 4 KiB page-size profile
  putLe32(b, kernelOffset + 0x38, 0x644D5241u); // ARM64 Image magic
  putMagic(b, ramdiskOffset, "NCV-INITRAMFS!!", 15);
  std::memcpy(b.data() + dtbOffset, dtb.data(), dtb.size());
  return b;
}

std::vector<uint8_t> makeBootV3() {
  const uint32_t kernelSize = 64, ramdiskSize = 9;
  std::vector<uint8_t> b(4096 + 4096 + 4096, 0);
  putMagic(b, 0, "ANDROID!", 8);
  putLe32(b, 8, kernelSize); putLe32(b, 12, ramdiskSize); putLe32(b, 20, 1580); putLe32(b, 40, 3);
  putLe32(b, 4096 + 56, 0x644D5241u);
  putMagic(b, 8192, "GENERICRD", ramdiskSize);
  return b;
}
std::vector<uint8_t> makeBootV4NoRamdisk() {
  std::vector<uint8_t> b(2 * 4096, 0);
  putMagic(b, 0, "ANDROID!", 8);
  putLe32(b, 8, 64); putLe32(b, 12, 0); putLe32(b, 20, 1584); putLe32(b, 40, 4);
  putLe32(b, 4096 + 56, 0x644D5241u);
  return b;
}
std::vector<uint8_t> makeInitBootV4() {
  std::vector<uint8_t> b(2 * 4096, 0);
  putMagic(b, 0, "ANDROID!", 8);
  putLe32(b, 8, 0); putLe32(b, 12, 12); putLe32(b, 20, 1584); putLe32(b, 40, 4);
  putMagic(b, 4096, "INIT-RAMDISK!", 12);
  return b;
}
std::vector<uint8_t> makeVendorV3() {
  std::vector<uint8_t> b(4096 + 4096 + 4096, 0);
  putMagic(b, 0, "VNDRBOOT", 8); putLe32(b, 8, 3); putLe32(b, 12, 4096);
  putLe32(b, 24, 7); putLe32(b, 2096, 2112); putLe32(b, 2100, 16); putLe64(b, 2104, 0x40000000);
  putMagic(b, 4096, "VENDORRD", 7);
  putMagic(b, 8192, "NCV-DTB-TEST!!!", 15);
  return b;
}
std::vector<uint8_t> makeVendorV4() {
  std::vector<uint8_t> b(5 * 4096, 0);
  putMagic(b, 0, "VNDRBOOT", 8); putLe32(b, 8, 4); putLe32(b, 12, 4096);
  putLe32(b, 24, 4); putLe32(b, 2096, 2128); putLe32(b, 2100, 4); putLe64(b, 2104, 0x40000000);
  putLe32(b, 2112, 108); putLe32(b, 2116, 1); putLe32(b, 2120, 108); putLe32(b, 2124, 8);
  putMagic(b, 4096, "VRD!", 4); putMagic(b, 8192, "DTB!", 4);
  putLe32(b, 12288, 4); // one table entry's size
  putLe32(b, 12292, 0); putLe32(b, 12296, 1);
  putMagic(b, 16384, "android=1", 8);
  return b;
}
}

NT_TEST(android_boot_parser_reads_v3_kernel_ramdisk_and_allows_generic_board_without_vendor) {
  auto boot = makeBootV3();
  AndroidBootImageInfo i;
  CHECK_ST(inspectAndroidBootImage(boot.data(), boot.size(), &i), Status::Ok);
  CHECK_EQ(i.headerVersion, 3ull); CHECK_EQ(i.pageSize, 4096ull);
  CHECK_EQ(i.kernelOffset, 4096ull); CHECK_EQ(i.kernelSize, 64ull);
  CHECK_EQ(i.ramdiskOffset, 8192ull); CHECK_EQ(i.ramdiskSize, 9ull);
  CHECK(i.requiresVendorBoot);
  AndroidBootBundle bundle;
  CHECK_ST(extractAndroidBootBundle(boot.data(), boot.size(), nullptr, 0, &bundle), Status::Ok);
  CHECK_EQ(bundle.kernel.size(), 64ull);
  CHECK_EQ(bundle.genericRamdisk.size(), 9ull);
}

NT_TEST(android_vendor_boot_parser_reads_v3_vendor_ramdisk_and_dtb) {
  auto vendor = makeVendorV3();
  AndroidVendorBootImageInfo i;
  CHECK_ST(inspectVendorBootImage(vendor.data(), vendor.size(), &i), Status::Ok);
  CHECK_EQ(i.headerVersion, 3ull); CHECK_EQ(i.pageSize, 4096ull);
  CHECK_EQ(i.vendorRamdiskOffset, 4096ull); CHECK_EQ(i.vendorRamdiskSize, 7ull);
  CHECK_EQ(i.dtbOffset, 8192ull); CHECK_EQ(i.dtbSize, 16ull);
}

NT_TEST(android_vendor_boot_parser_reads_v4_table_and_bootconfig) {
  auto vendor = makeVendorV4();
  AndroidVendorBootImageInfo i;
  CHECK_ST(inspectVendorBootImage(vendor.data(), vendor.size(), &i), Status::Ok);
  CHECK_EQ(i.headerVersion, 4ull); CHECK_EQ(i.ramdiskTableOffset, 12288ull);
  CHECK_EQ(i.ramdiskTableSize, 108ull); CHECK_EQ(i.ramdiskTableEntryCount, 1ull);
  CHECK_EQ(i.bootConfigOffset, 16384ull); CHECK_EQ(i.bootConfigSize, 8ull);
}

NT_TEST(android13_boot_bundle_uses_init_boot_for_generic_ramdisk) {
  auto boot = makeBootV4NoRamdisk();
  auto vendor = makeVendorV4();
  auto initBoot = makeInitBootV4();
  AndroidBootImageInfo bi, ii;
  CHECK_ST(inspectAndroidBootImage(boot.data(), boot.size(), &bi), Status::Ok);
  CHECK_EQ(bi.kernelSize, 64ull); CHECK_EQ(bi.ramdiskSize, 0ull); CHECK(bi.requiresVendorBoot);
  CHECK_ST(inspectAndroidBootImage(initBoot.data(), initBoot.size(), &ii), Status::Ok);
  CHECK_EQ(ii.kernelSize, 0ull); CHECK_EQ(ii.ramdiskSize, 12ull); CHECK(!ii.requiresVendorBoot);
  AndroidBootBundle bundle;
  CHECK_ST(extractAndroidBootBundle(boot.data(), boot.size(), vendor.data(), vendor.size(),
                                    initBoot.data(), initBoot.size(), &bundle), Status::Ok);
  CHECK_EQ(bundle.kernel.size(), 64ull); CHECK_EQ(bundle.genericRamdisk.size(), 12ull);
  CHECK_EQ(bundle.vendorRamdisk.size(), 4ull);
}

NT_TEST(android_boot_parser_rejects_truncated_or_invalid_offsets) {
  auto boot = makeBootV3();
  AndroidBootImageInfo i;
  CHECK_ST(inspectAndroidBootImage(boot.data(), 100, &i), Status::InvalidArgument);
  putLe32(boot, 8, 0xFFFFFFF0u);
  CHECK_ST(inspectAndroidBootImage(boot.data(), boot.size(), &i), Status::OutOfBounds);
  auto vendor = makeVendorV3();
  putLe32(vendor, 24, 0xFFFFFFFFu);
  AndroidVendorBootImageInfo vi;
  CHECK_ST(inspectVendorBootImage(vendor.data(), vendor.size(), &vi), Status::OutOfBounds);
}

NT_TEST(android_initramfs_extractor_combines_actual_ramdisk_fragments_without_oem_kernel) {
  auto boot = makeBootV3();
  std::memcpy(boot.data() + 44, "console=ttyMT0 androidboot.hardware=mt6768", 42);
  auto vendor = makeVendorV3();
  std::memcpy(vendor.data() + 28, "androidboot.hardware=mt6768", 27);
  std::vector<uint8_t> initrd;
  std::string cmdline;
  CHECK_ST(extractAndroidInitramfs(boot.data(), boot.size(), vendor.data(), vendor.size(),
                                   nullptr, 0, &initrd, &cmdline), Status::Ok);
  CHECK_EQ(initrd.size(), 16ull);
  CHECK(std::memcmp(initrd.data(), "VENDORR", 7) == 0);
  CHECK(std::memcmp(initrd.data() + 7, "GENERICRD", 9) == 0);
  CHECK(cmdline.find("console=ttyMT0") != std::string::npos);
  CHECK(cmdline.find("androidboot.hardware=mt6768") != std::string::npos);
}

NT_TEST(android_initramfs_extractor_appends_valid_bootconfig_trailer) {
  auto boot = makeBootV4NoRamdisk();
  auto vendor = makeVendorV4();
  auto initBoot = makeInitBootV4();
  std::vector<uint8_t> initrd;
  std::string cmdline;
  CHECK_ST(extractAndroidInitramfs(boot.data(), boot.size(), vendor.data(), vendor.size(),
                                   initBoot.data(), initBoot.size(), &initrd, &cmdline), Status::Ok);
  const std::string expectedPrefix = "VRD!INIT-RAMDISK";
  CHECK(initrd.size() > expectedPrefix.size() + 20);
  CHECK(std::memcmp(initrd.data(), expectedPrefix.data(), expectedPrefix.size()) == 0);
  const uint8_t bootConfigMagic[] = {'#','B','O','O','T','C','O','N','F','I','G','\n'};
  CHECK(std::search(initrd.begin(), initrd.end(), bootConfigMagic,
                    bootConfigMagic + sizeof(bootConfigMagic)) != initrd.end());
}

NT_TEST(android_boot_bundle_extracts_v3_components_and_cmdline) {
  auto boot = makeBootV3();
  std::memcpy(boot.data() + 44, "console=ttyAMA0", 15);
  auto vendor = makeVendorV3();
  std::memcpy(vendor.data() + 28, "androidboot.hardware=ncv", 24);
  AndroidBootBundle bundle;
  CHECK_ST(extractAndroidBootBundle(boot.data(), boot.size(), vendor.data(), vendor.size(), &bundle), Status::Ok);
  CHECK_EQ(bundle.headerVersion, 3ull); CHECK(bundle.hasVendorBoot);
  CHECK_EQ(bundle.kernel.size(), 64ull); CHECK_EQ(bundle.genericRamdisk.size(), 9ull);
  CHECK_EQ(bundle.vendorRamdisk.size(), 7ull); CHECK_EQ(bundle.vendorDtb.size(), 16ull);
  CHECK(bundle.commandLine.find("console=ttyAMA0") != std::string::npos);
  CHECK(bundle.commandLine.find("androidboot.hardware=ncv") != std::string::npos);
}


NT_TEST(android_monolithic_boot_img_loads_without_vendor_boot_component) {
  auto boot = makeBootV3();
  const char physicalConsoleArgs[] = "androidboot.hardware=mt6768 console=ttyMT0 earlycon=mtk_uart root=/dev/ram";
  std::memcpy(boot.data() + 44, physicalConsoleArgs, sizeof(physicalConsoleArgs));
  BoardConfig cfg;
  cfg.ramSize = 32ull << 20;
  Status createStatus = Status::Ok;
  auto vm = Vm::create(cfg, &createStatus);
  CHECK(vm != nullptr);
  CHECK_ST(createStatus, Status::Ok);
  AndroidBootLoadInfo info;
  CHECK_ST(vm->loadAndroidBootBundle(boot.data(), boot.size(), nullptr, 0, nullptr, 0, &info), Status::Ok);
  CHECK(info.entry != 0);
  CHECK(info.initrdAddress != 0);
  CHECK(info.initrdSize >= 9ull);
  CHECK(info.commandLine.find("androidboot.hardware=mt6768") != std::string::npos);
  CHECK(info.commandLine.find("console=ttyAMA0") != std::string::npos);
  CHECK(info.commandLine.find("earlycon=pl011,mmio32,0x09000000") != std::string::npos);
  CHECK(info.commandLine.find("ttyMT0") == std::string::npos);
  CHECK(info.commandLine.find("mtk_uart") == std::string::npos);
  GuestAddr dtb;
  CHECK_ST(vm->prepareLinuxBootDtb(info.commandLine, &dtb, info.initrdAddress, info.initrdSize), Status::Ok);
  CHECK(dtb.value != 0);
}


NT_TEST(android_boot_v3_vm_selects_vendor_boot_dtb_when_boot_has_none) {
  auto boot = makeBootV3();
  auto vendor = makeVendorV3();
  BoardConfig cfg;
  cfg.ramBase = 0x40000000ull;
  cfg.ramSize = 32ull << 20;
  Status createStatus = Status::Ok;
  auto vm = Vm::create(cfg, &createStatus);
  CHECK(vm != nullptr);
  CHECK_ST(createStatus, Status::Ok);

  AndroidBootLoadInfo info;
  CHECK_ST(vm->loadAndroidBootBundle(boot.data(), boot.size(), vendor.data(), vendor.size(), nullptr, 0, &info), Status::Ok);
  CHECK_EQ(info.bootDtbSize, 0ull);
  CHECK_EQ(info.vendorDtbSize, 16ull);
  CHECK_EQ(info.selectedDtbSize, info.vendorDtbSize);
  CHECK(info.selectedDtbFromVendorBoot);
}

NT_TEST(android_boot_v2_embedded_dtb_is_extracted_and_adapted_for_ncv_board) {
  LinuxDtbConfig sourceConfig;
  sourceConfig.ramBase = 0x40000000ull;
  sourceConfig.ramSize = 64ull << 20;
  std::vector<uint8_t> sourceDtb;
  CHECK_ST(buildLinuxDtb(sourceConfig, &sourceDtb), Status::Ok);
  auto boot = makeBootV2WithDtb(sourceDtb);

  AndroidBootImageInfo header;
  CHECK_ST(inspectAndroidBootImage(boot.data(), boot.size(), &header), Status::Ok);
  CHECK_EQ(header.kernelHeaderAddress, 0x40080000ull);
  CHECK_EQ(header.ramdiskHeaderAddress, 0x44000000ull);
  CHECK_EQ(header.tagsHeaderAddress, 0x40000100ull);
  CHECK_EQ(header.dtbHeaderAddress, 0x41000000ull);
  AndroidBootBundle bundle;
  CHECK_ST(extractAndroidBootBundle(boot.data(), boot.size(), nullptr, 0, &bundle), Status::Ok);
  CHECK_EQ(bundle.headerVersion, 2ull);
  CHECK_EQ(bundle.kernelHeaderAddress, 0x40080000ull);
  CHECK_EQ(bundle.ramdiskHeaderAddress, 0x44000000ull);
  CHECK_EQ(bundle.tagsHeaderAddress, 0x40000100ull);
  CHECK_EQ(bundle.dtbHeaderAddress, 0x41000000ull);
  CHECK_EQ(bundle.bootDtb.size(), sourceDtb.size());
  CHECK(std::memcmp(bundle.bootDtb.data(), sourceDtb.data(), 4) == 0);

  LinuxDtbConfig targetConfig;
  targetConfig.ramBase = 0x40000000ull;
  targetConfig.ramSize = 512ull << 20;
  targetConfig.virtioBlockCount = 2;
  std::vector<uint8_t> hybrid;
  std::string report;
  CHECK_ST(buildHybridAndroidDtb(bundle.bootDtb.data(), bundle.bootDtb.size(), targetConfig, &hybrid, &report), Status::Ok);
  CHECK(hybrid.size() > 40);
  const uint8_t fdtMagic[] = {0xD0, 0x0D, 0xFE, 0xED};
  CHECK(std::memcmp(hybrid.data(), fdtMagic, sizeof(fdtMagic)) == 0);
  CHECK(report.find("source=raw FDT") != std::string::npos);
  CHECK(report.find("virtio-blocks=2") != std::string::npos);
  CHECK(std::search(hybrid.begin(), hybrid.end(), "virtio,mmio", "virtio,mmio" + 11) != hybrid.end());
  CHECK(std::search(hybrid.begin(), hybrid.end(), "arm,pl011", "arm,pl011" + 9) != hybrid.end());
  std::vector<uint8_t> roundTrip;
  std::string roundTripReport;
  CHECK_ST(buildHybridAndroidDtb(hybrid.data(), hybrid.size(), targetConfig, &roundTrip, &roundTripReport), Status::Ok);
  CHECK(roundTrip.size() > 40);
  CHECK(roundTripReport.find("source=raw FDT") != std::string::npos);
}

NT_TEST(android_hybrid_dtb_preserves_explicit_boot_stack_reservation) {
  LinuxDtbConfig sourceConfig;
  sourceConfig.ramBase = 0x40000000ull;
  sourceConfig.ramSize = 64ull << 20;
  std::vector<uint8_t> source;
  CHECK_ST(buildLinuxDtb(sourceConfig, &source), Status::Ok);

  LinuxDtbConfig targetConfig;
  targetConfig.ramBase = 0x40000000ull;
  targetConfig.ramSize = 512ull << 20;
  targetConfig.bootScratchReserveBase = 0x5FFD0000ull;
  targetConfig.bootScratchReserveSize = 64ull << 10;
  std::vector<uint8_t> hybrid;
  std::string report;
  CHECK_ST(buildHybridAndroidDtb(source.data(), source.size(), targetConfig, &hybrid, &report), Status::Ok);
  CHECK(fdtHasReserve(hybrid, targetConfig.bootScratchReserveBase, targetConfig.bootScratchReserveSize));
  CHECK(report.find("boot-stack-scratch-reserved=0x5ffd0000+0x10000") != std::string::npos);
}

NT_TEST(android_boot_v2_vm_reserves_stack_scratch_range_below_final_dtb) {
  LinuxDtbConfig sourceConfig;
  sourceConfig.ramBase = 0x40000000ull;
  sourceConfig.ramSize = 64ull << 20;
  std::vector<uint8_t> sourceDtb;
  CHECK_ST(buildLinuxDtb(sourceConfig, &sourceDtb), Status::Ok);
  auto boot = makeBootV2WithDtb(sourceDtb);

  BoardConfig cfg;
  cfg.ramBase = 0x40000000ull;
  cfg.ramSize = 64ull << 20;
  Status createStatus = Status::Ok;
  auto vm = Vm::create(cfg, &createStatus);
  CHECK(vm != nullptr);
  CHECK_ST(createStatus, Status::Ok);
  AndroidBootLoadInfo info;
  CHECK_ST(vm->loadAndroidBootBundle(boot.data(), boot.size(), nullptr, 0, nullptr, 0, &info), Status::Ok);
  CHECK_EQ(info.kernelHeaderAddress, 0x40080000ull);
  CHECK_EQ(info.ramdiskHeaderAddress, 0x44000000ull);
  CHECK_EQ(info.tagsHeaderAddress, 0x40000100ull);
  CHECK_EQ(info.dtbHeaderAddress, 0x41000000ull);
  GuestAddr dtb;
  CHECK_ST(vm->prepareAndroidBootDtb(info.commandLine, &dtb, info.initrdAddress, info.initrdSize), Status::Ok);

  const std::string report = vm->compatibilityReport();
  const std::string marker = "boot-stack-scratch-reserved=0x";
  const size_t at = report.find(marker);
  CHECK(at != std::string::npos);
  if (at != std::string::npos) {
    const size_t baseBegin = at + marker.size();
    const size_t plus = report.find("+0x", baseBegin);
    CHECK(plus != std::string::npos);
    if (plus != std::string::npos) {
      const uint64_t base = std::stoull(report.substr(baseBegin, plus - baseBegin), nullptr, 16);
      const size_t sizeBegin = plus + 3;
      const size_t end = report.find_first_of(";\n", sizeBegin);
      const uint64_t size = std::stoull(report.substr(sizeBegin, end - sizeBegin), nullptr, 16);
      CHECK_EQ(size, 64ull << 10);
      CHECK_EQ(base + size, dtb.value);
    }
  }
}

