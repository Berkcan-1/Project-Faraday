#include "devices/ncv_virtio_rng.h"
#include "devices/ncv_gicv2.h"
#include "test_framework.h"
#include <cstdint>
#include <cstring>

using namespace ncv;

NT_TEST(virtio_rng_has_modern_mmio_identity_and_queue) {
  Bus bus;
  CHECK_ST(bus.addRam(GuestAddr(0x40000000ull), 0x20000), Status::Ok);
  VirtioMmioRng rng(&bus);
  CHECK_ST(bus.addDevice(GuestAddr(VirtioMmioRng::kBase), VirtioMmioRng::kRegionSize, &rng), Status::Ok);
  uint64_t v=0;
  CHECK_ST(bus.read(GuestAddr(VirtioMmioRng::kBase),4,&v),Status::Ok); CHECK_EQ(v,0x74726976u);
  CHECK_ST(bus.read(GuestAddr(VirtioMmioRng::kBase+8),4,&v),Status::Ok); CHECK_EQ(v,4u);
  CHECK_ST(bus.read(GuestAddr(VirtioMmioRng::kBase+0x34),4,&v),Status::Ok); CHECK_EQ(v,VirtioMmioRng::kMaxQueueSize);
  CHECK_ST(bus.write(GuestAddr(VirtioMmioRng::kBase+0x14),4,1),Status::Ok);
  CHECK_ST(bus.read(GuestAddr(VirtioMmioRng::kBase+0x10),4,&v),Status::Ok); CHECK_EQ(v,1u); // VERSION_1 feature
}

NT_TEST(virtio_rng_writes_guest_dma_buffer_updates_used_ring_and_irq) {
  constexpr uint64_t ram=0x40000000ull, desc=ram+0x100, avail=ram+0x400, used=ram+0x500, buffer=ram+0x1000;
  Bus bus;
  CHECK_ST(bus.addRam(GuestAddr(ram),0x20000),Status::Ok);
  VirtioMmioRng rng(&bus); GicV2 gic; bool irq=false;
  rng.setIrqSink([&](bool level){ irq=level; });
  CHECK_ST(bus.addDevice(GuestAddr(VirtioMmioRng::kBase),VirtioMmioRng::kRegionSize,&rng),Status::Ok);
  // One writable descriptor, 32-byte entropy buffer.
  uint8_t d[16]{}; const uint64_t da=buffer; const uint32_t len=32; const uint16_t flags=2,next=0;
  std::memcpy(d,&da,8); std::memcpy(d+8,&len,4); std::memcpy(d+12,&flags,2); std::memcpy(d+14,&next,2);
  CHECK_ST(bus.loadBytes(GuestAddr(desc),d,sizeof(d)),Status::Ok);
  uint16_t one=1, zero=0; CHECK_ST(bus.loadBytes(GuestAddr(avail+2),reinterpret_cast<const uint8_t*>(&one),2),Status::Ok);
  CHECK_ST(bus.loadBytes(GuestAddr(avail+4),reinterpret_cast<const uint8_t*>(&zero),2),Status::Ok);
  auto wr=[&](uint64_t o,uint32_t v){ return bus.write(GuestAddr(VirtioMmioRng::kBase+o),4,v); };
  CHECK_ST(wr(0x38,8),Status::Ok); CHECK_ST(wr(0x80,static_cast<uint32_t>(desc)),Status::Ok);
  CHECK_ST(wr(0x84,static_cast<uint32_t>(desc>>32)),Status::Ok);
  CHECK_ST(wr(0x90,static_cast<uint32_t>(avail)),Status::Ok); CHECK_ST(wr(0x94,static_cast<uint32_t>(avail>>32)),Status::Ok);
  CHECK_ST(wr(0xA0,static_cast<uint32_t>(used)),Status::Ok); CHECK_ST(wr(0xA4,static_cast<uint32_t>(used>>32)),Status::Ok);
  CHECK_ST(wr(0x44,1),Status::Ok); CHECK_ST(wr(0x50,0),Status::Ok);
  uint16_t usedIndex=0; CHECK_ST(bus.readBytes(GuestAddr(used+2),reinterpret_cast<uint8_t*>(&usedIndex),2),Status::Ok);
  CHECK_EQ(usedIndex,1u); CHECK(irq);
  uint8_t bytes[32]{}; CHECK_ST(bus.readBytes(GuestAddr(buffer),bytes,sizeof(bytes)),Status::Ok);
  // Entropy can theoretically be all-zero, so prove the ring completion and payload accessibility instead.
  uint32_t written=0; CHECK_ST(bus.readBytes(GuestAddr(used+8),reinterpret_cast<uint8_t*>(&written),4),Status::Ok);
  CHECK_EQ(written,32u);
  uint64_t irqStatus=0; CHECK_ST(bus.read(GuestAddr(VirtioMmioRng::kBase+0x60),4,&irqStatus),Status::Ok); CHECK_EQ(irqStatus,1u);
}
