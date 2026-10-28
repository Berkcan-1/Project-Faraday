#include "devices/ncv_gicv2.h"
#include "test_framework.h"

using namespace ncv;

NT_TEST(gicv2_reports_256_interrupt_ids) {
  GicV2 gic;
  uint64_t value = 0;
  CHECK_ST(gic.read(0x004, 4, &value), Status::Ok);
  CHECK_EQ(value & 0x1Fu, 7u);
}

NT_TEST(gicv2_supports_exact_failing_is_enabler_bank_7_physical_address) {
  GicV2 gic;
  Bus bus;
  CHECK_ST(bus.addDevice(GuestAddr(0x08000000u), GicV2::kRegionSize, &gic), Status::Ok);
  CHECK_ST(bus.write(GuestAddr(0x0800011Cu), 4, 0xA5A55A5Au), Status::Ok);
  uint64_t value = 0;
  CHECK_ST(bus.read(GuestAddr(0x0800011Cu), 4, &value), Status::Ok);
  CHECK_EQ(value, 0xA5A55A5Au);
  CHECK_ST(bus.write(GuestAddr(0x0800019Cu), 4, 0x00000002u), Status::Ok);
  CHECK_ST(bus.read(GuestAddr(0x0800011Cu), 4, &value), Status::Ok);
  CHECK_EQ(value, 0xA5A55A58u);
}

NT_TEST(gicv2_supports_high_pending_active_priority_target_and_config_banks) {
  GicV2 gic;
  uint64_t value = 0;
  CHECK_ST(gic.write(0x21C, 4, 0x80000000u), Status::Ok);
  CHECK_ST(gic.read(0x21C, 4, &value), Status::Ok);
  CHECK_EQ(value, 0x80000000u);
  CHECK_ST(gic.write(0x31C, 4, 0x40000000u), Status::Ok);
  CHECK_ST(gic.read(0x31C, 4, &value), Status::Ok);
  CHECK_EQ(value, 0x40000000u);
  CHECK_ST(gic.write(0x4FC, 4, 0x44332211u), Status::Ok);
  CHECK_ST(gic.read(0x4FC, 4, &value), Status::Ok);
  CHECK_EQ(value, 0x44332211u);
  CHECK_ST(gic.write(0x8FC, 4, 0x01010101u), Status::Ok);
  CHECK_ST(gic.read(0x8FC, 4, &value), Status::Ok);
  CHECK_EQ(value, 0x01010101u);
  CHECK_ST(gic.write(0xC3C, 4, 0xAAAAAAAAu), Status::Ok);
  CHECK_ST(gic.read(0xC3C, 4, &value), Status::Ok);
  CHECK_EQ(value, 0xAAAAAAAAu);
}

NT_TEST(gicv2_can_deliver_an_interrupt_from_bank_7) {
  GicV2 gic;
  uint64_t value = 0;
  CHECK_ST(gic.write(0x000, 4, 1), Status::Ok); // GICD_CTLR
  CHECK_ST(gic.write(0x10000, 4, 1), Status::Ok); // GICC_CTLR
  CHECK_ST(gic.write(0x004, 4, 0xFF), Status::Ok); // GICC_PMR
  CHECK_ST(gic.write(0x11C, 4, 0x80000000u), Status::Ok); // enable IRQ 255
  CHECK_ST(gic.write(0x21C, 4, 0x80000000u), Status::Ok); // pend IRQ 255
  CHECK(gic.hasDeliverableInterrupt());
  CHECK_EQ(gic.acknowledge(), 255u);
  CHECK_ST(gic.read(0x31C, 4, &value), Status::Ok);
  CHECK_EQ(value & 0x80000000u, 0x80000000u);
  CHECK_ST(gic.write(0x10010, 4, 255), Status::Ok); // GICC_EOIR
  CHECK_ST(gic.read(0x31C, 4, &value), Status::Ok);
  CHECK_EQ(value & 0x80000000u, 0u);
}

NT_TEST(gicv2_reserved_aligned_registers_do_not_raise_external_device_abort) {
  GicV2 gic;
  uint64_t value = 0xFFFFFFFFu;
  CHECK_ST(gic.write(0x0A0, 4, 0x12345678u), Status::Ok);
  CHECK_ST(gic.read(0x0A0, 4, &value), Status::Ok);
  CHECK_EQ(value, 0u);
}


NT_TEST(gicv2_accepts_architected_byte_accesses_for_priority_and_target_registers) {
  GicV2 gic;
  Bus bus;
  CHECK_ST(bus.addDevice(GuestAddr(0x08000000u), GicV2::kRegionSize, &gic), Status::Ok);

  // Exact address from the guest abort: GICD_ITARGETSR byte for SPI/IRQ 239.
  CHECK_ST(bus.write(GuestAddr(0x080008EFu), 1, 1u), Status::Ok);
  uint64_t value = 0;
  CHECK_ST(bus.read(GuestAddr(0x080008EFu), 1, &value), Status::Ok);
  CHECK_EQ(value, 1u);

  // GICD_IPRIORITYR is byte-accessible too.
  CHECK_ST(bus.write(GuestAddr(0x080004EFu), 1, 0x60u), Status::Ok);
  CHECK_ST(bus.read(GuestAddr(0x080004EFu), 1, &value), Status::Ok);
  CHECK_EQ(value, 0x60u);
}

NT_TEST(gicv2_byte_accesses_remain_restricted_to_architected_byte_registers) {
  GicV2 gic;
  uint64_t value = 0;
  CHECK_ST(gic.write(0x000, 1, 1u), Status::InvalidArgument); // GICD_CTLR is word-only
  CHECK_ST(gic.read(0x000, 1, &value), Status::InvalidArgument);
  CHECK_ST(gic.write(0x8EF, 4, 1u), Status::InvalidArgument); // unaligned word still invalid
}


NT_TEST(gicv2_accepts_byte_accesses_for_sgi_pending_source_registers) {
  GicV2 gic;
  uint64_t value = 0;
  CHECK_ST(gic.write(0xF21, 1, 1u), Status::Ok); // SPENDSGIR byte for SGI 1, source CPU 0
  CHECK_ST(gic.read(0xF21, 1, &value), Status::Ok);
  CHECK_EQ(value, 1u);
  CHECK_ST(gic.write(0xF11, 1, 1u), Status::Ok); // CPENDSGIR byte clears SGI 1 from CPU 0
  CHECK_ST(gic.read(0xF21, 1, &value), Status::Ok);
  CHECK_EQ(value, 0u);
}
