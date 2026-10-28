#include "core/ncv_vm.h"
#include "devices/ncv_framebuffer.h"
#include "devices/ncv_gicv2.h"
#include "test_framework.h"

using namespace ncv;

NT_TEST(mtk_probe_profile_returns_zero_for_unmapped_read_and_ignores_write) {
  BoardConfig cfg;
  cfg.ramSize = 32ull << 20;
  cfg.compatibilityProfileId = 1;
  Status status = Status::Ok;
  auto vm = Vm::create(cfg, &status);
  CHECK(vm != nullptr);
  CHECK_ST(status, Status::Ok);
  uint64_t value = 0xDEADBEEF;
  CHECK_ST(vm->bus().read(GuestAddr(0x11002000ull), 4, &value), Status::Ok);
  CHECK_EQ(value, 0ull);
  CHECK_ST(vm->bus().write(GuestAddr(0x11002004ull), 4, 0x12345678), Status::Ok);
  const auto report = vm->compatibilityReport();
  CHECK(report.find("emulated-window reads=1") != std::string::npos);
  CHECK(report.find("ignored writes=1") != std::string::npos);
}

NT_TEST(strict_profile_keeps_unmapped_access_faulting) {
  BoardConfig cfg;
  cfg.ramSize = 32ull << 20;
  Status status = Status::Ok;
  auto vm = Vm::create(cfg, &status);
  CHECK(vm != nullptr);
  CHECK_ST(status, Status::Ok);
  uint64_t value = 0;
  CHECK_ST(vm->bus().read(GuestAddr(0x11002000ull), 4, &value), Status::Unmapped);
}

NT_TEST(mtk_probe_profile_does_not_swallow_unmapped_access_outside_heuristic_aperture) {
  BoardConfig cfg;
  cfg.ramSize = 32ull << 20;
  cfg.compatibilityProfileId = 1;
  Status status = Status::Ok;
  auto vm = Vm::create(cfg, &status);
  CHECK(vm != nullptr);
  CHECK_ST(status, Status::Ok);
  uint64_t value = 0;
  CHECK_ST(vm->bus().read(GuestAddr(0x30000000ull), 4, &value), Status::Unmapped);
}


NT_TEST(mtk_probe_window_does_not_overlap_ncv_framebuffer) {
  constexpr uint64_t mtkFallbackStart = 0x10000000ull;
  constexpr uint64_t mtkFallbackEnd = 0x20000000ull;
  const uint64_t fbStart = GuestFramebuffer::kBase;
  const uint64_t fbEnd = fbStart + GuestFramebuffer::kRegionSize;
  CHECK(fbEnd <= mtkFallbackStart || fbStart >= mtkFallbackEnd);
}

NT_TEST(mtk_probe_records_rejected_unmapped_accesses_outside_fallback_aperture) {
  BoardConfig cfg;
  cfg.ramSize = 32ull << 20;
  cfg.compatibilityProfileId = 1;
  Status status = Status::Ok;
  auto vm = Vm::create(cfg, &status);
  CHECK(vm != nullptr);
  CHECK_ST(status, Status::Ok);
  uint64_t value = 0;
  CHECK_ST(vm->bus().read(GuestAddr(0x30000000ull), 4, &value), Status::Unmapped);
  CHECK_ST(vm->bus().write(GuestAddr(0x30000008ull), 4, 0xDEADBEEFu), Status::Unmapped);
  const auto report = vm->mmioProbeReport();
  CHECK(report.find("rejected unmapped reads=1") != std::string::npos);
  CHECK(report.find("rejected unmapped writes=1") != std::string::npos);
  CHECK(report.find("r[4] @0x30000000") != std::string::npos);
  CHECK(report.find("w[4] @0x30000008 value=0xdeadbeef") != std::string::npos);
}


NT_TEST(gicv2_cpu_interface_apr_registers_accept_guest_writes) {
  Bus bus;
  GicV2 gic;
  CHECK_ST(bus.addDevice(GuestAddr(0x08000000ull), GicV2::kRegionSize, &gic), Status::Ok);

  // Exact address from the device log: GICC base 0x08010000 + APR0 offset 0xD0.
  CHECK_ST(bus.write(GuestAddr(0x080100D0ull), 4, 0x00000001u), Status::Ok);
  uint64_t value = 0;
  CHECK_ST(bus.read(GuestAddr(0x080100D0ull), 4, &value), Status::Ok);
  CHECK_EQ(value, 0x00000001ull);

  // Remaining architected active-priority slots should not abort the guest either.
  CHECK_ST(bus.write(GuestAddr(0x080100D4ull), 4, 0x00000002u), Status::Ok);
  CHECK_ST(bus.write(GuestAddr(0x080100D8ull), 4, 0x00000004u), Status::Ok);
  CHECK_ST(bus.write(GuestAddr(0x080100DCull), 4, 0x00000008u), Status::Ok);
  CHECK_ST(bus.write(GuestAddr(0x080100E0ull), 4, 0x00000010u), Status::Ok);
  CHECK_ST(bus.read(GuestAddr(0x080100E0ull), 4, &value), Status::Ok);
  CHECK_EQ(value, 0x00000010ull);
}

NT_TEST(gicv2_cpu_interface_dir_register_is_accepted) {
  Bus bus;
  GicV2 gic;
  CHECK_ST(bus.addDevice(GuestAddr(0x08000000ull), GicV2::kRegionSize, &gic), Status::Ok);
  CHECK_ST(bus.write(GuestAddr(0x08011000ull), 4, 1023u), Status::Ok);
}
