#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include "devices/ncv_virtio_blk.h"
#include "test_framework.h"

using namespace ncv;
namespace {
void put16(std::vector<uint8_t>& b, size_t o, uint16_t v) { b[o] = static_cast<uint8_t>(v); b[o + 1] = static_cast<uint8_t>(v >> 8); }
void put32(std::vector<uint8_t>& b, size_t o, uint32_t v) { for (unsigned i=0;i<4;i++) b[o+i]=static_cast<uint8_t>(v>>(i*8)); }
void put64(std::vector<uint8_t>& b, size_t o, uint64_t v) { put32(b,o,static_cast<uint32_t>(v)); put32(b,o+4,static_cast<uint32_t>(v>>32)); }
uint64_t get64(const uint8_t* p) { uint64_t v = 0; for (unsigned i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8u * i); return v; }
int makeTemp(const std::vector<uint8_t>& contents) {
  char path[] = "/tmp/ncvblkXXXXXX";
  int fd = mkstemp(path);
  if (fd >= 0) {
    unlink(path);
    if (!contents.empty() && write(fd, contents.data(), contents.size()) != static_cast<ssize_t>(contents.size())) { close(fd); return -1; }
    lseek(fd, 0, SEEK_SET);
  }
  return fd;
}
}

NT_TEST(virtio_mmio_block_exposes_v2_identity_and_capacity) {
  std::vector<uint8_t> disk(4096, 0x5A);
  int fd = makeTemp(disk); CHECK(fd >= 0); if (fd < 0) return;
  Bus bus(1ull << 20); CHECK_ST(bus.addRam(GuestAddr(0x40000000), 64ull << 10), Status::Ok);
  std::unique_ptr<VirtioMmioBlock> dev;
  CHECK_ST(VirtioMmioBlock::openFd(&bus, fd, true, &dev), Status::Ok);
  if (dev) {
    uint64_t v=0;
    CHECK_ST(dev->read(0x000,4,&v),Status::Ok); CHECK_EQ(v,0x74726976ull);
    CHECK_ST(dev->read(0x004,4,&v),Status::Ok); CHECK_EQ(v,2ull);
    CHECK_ST(dev->read(0x008,4,&v),Status::Ok); CHECK_EQ(v,2ull);
    CHECK_ST(dev->read(0x100,4,&v),Status::Ok); CHECK_EQ(v,8ull);
    CHECK(dev->readOnly()); CHECK(!dev->sparseImage());
  }
  close(fd);
}

NT_TEST(virtio_mmio_block_serves_a_read_queue_and_asserts_irq) {
  std::vector<uint8_t> disk(4096);
  for (size_t i=0;i<disk.size();++i) disk[i]=static_cast<uint8_t>(i & 0xFF);
  int fd = makeTemp(disk); CHECK(fd >= 0); if (fd < 0) return;
  constexpr uint64_t ram=0x40000000ull, mmio=0x0A000000ull;
  Bus bus(1ull << 20); CHECK_ST(bus.addRam(GuestAddr(ram), 64ull << 10), Status::Ok);
  std::unique_ptr<VirtioMmioBlock> dev;
  CHECK_ST(VirtioMmioBlock::openFd(&bus, fd, true, &dev), Status::Ok);
  close(fd); if (!dev) return;
  bool irq=false; dev->setIrqSink([&](bool level){ irq=level; });
  CHECK_ST(bus.addDevice(GuestAddr(mmio), VirtioMmioBlock::kRegionSize, dev.get()), Status::Ok);
  auto wr=[&](uint64_t off, uint32_t v){ return bus.write(GuestAddr(mmio+off),4,v); };

  constexpr uint64_t desc=ram+0x1000, avail=ram+0x2000, used=ram+0x3000;
  std::vector<uint8_t> descriptors(48,0);
  put64(descriptors,0,ram+0x4000); put32(descriptors,8,16); put16(descriptors,12,1); put16(descriptors,14,1);
  put64(descriptors,16,ram+0x5000); put32(descriptors,24,512); put16(descriptors,28,3); put16(descriptors,30,2);
  put64(descriptors,32,ram+0x6000); put32(descriptors,40,1); put16(descriptors,44,2); put16(descriptors,46,0);
  CHECK_ST(bus.loadBytes(GuestAddr(desc), descriptors.data(), descriptors.size()), Status::Ok);
  std::vector<uint8_t> request(16,0); put32(request,0,0); put64(request,8,0); // VIRTIO_BLK_T_IN, sector 0
  CHECK_ST(bus.loadBytes(GuestAddr(ram+0x4000), request.data(), request.size()), Status::Ok);
  std::vector<uint8_t> availRing(8,0); put16(availRing,2,1); put16(availRing,4,0);
  CHECK_ST(bus.loadBytes(GuestAddr(avail), availRing.data(), availRing.size()), Status::Ok);
  std::vector<uint8_t> zeros(32,0); CHECK_ST(bus.loadBytes(GuestAddr(used), zeros.data(), zeros.size()), Status::Ok);

  CHECK_ST(wr(0x024,1),Status::Ok); CHECK_ST(wr(0x020,1),Status::Ok); // negotiate VERSION_1
  CHECK_ST(wr(0x070,3),Status::Ok); CHECK_ST(wr(0x070,11),Status::Ok); CHECK_ST(wr(0x070,15),Status::Ok);
  CHECK_ST(wr(0x030,0),Status::Ok); CHECK_ST(wr(0x038,8),Status::Ok);
  CHECK_ST(wr(0x080,static_cast<uint32_t>(desc)),Status::Ok); CHECK_ST(wr(0x084,static_cast<uint32_t>(desc>>32)),Status::Ok);
  CHECK_ST(wr(0x090,static_cast<uint32_t>(avail)),Status::Ok); CHECK_ST(wr(0x094,static_cast<uint32_t>(avail>>32)),Status::Ok);
  CHECK_ST(wr(0x0a0,static_cast<uint32_t>(used)),Status::Ok); CHECK_ST(wr(0x0a4,static_cast<uint32_t>(used>>32)),Status::Ok);
  CHECK_ST(wr(0x044,1),Status::Ok); CHECK_ST(wr(0x050,0),Status::Ok);
  CHECK(irq);
  std::vector<uint8_t> received(512,0); CHECK_ST(bus.readBytes(GuestAddr(ram+0x5000),received.data(),received.size()),Status::Ok);
  CHECK_EQ(received[0],0ull); CHECK_EQ(received[255],255ull); CHECK_EQ(received[511],255ull);
  std::vector<uint8_t> status(1,0xFF); CHECK_ST(bus.readBytes(GuestAddr(ram+0x6000),status.data(),1),Status::Ok); CHECK_EQ(status[0],0ull);
  uint64_t usedIndex=0; CHECK_ST(bus.read(GuestAddr(used+2),2,&usedIndex),Status::Ok); CHECK_EQ(usedIndex,1ull);
  CHECK_ST(wr(0x064,1),Status::Ok); CHECK(!irq);
}

NT_TEST(virtio_mmio_block_reads_android_sparse_fill_and_dont_care_chunks) {
  // Android sparse: 2 blocks of RAW (0x41), 1 FILL block (bytes 01 02 03 04), 1 DONT_CARE block.
  const uint32_t block=512; std::vector<uint8_t> sparse(28,0);
  auto appendChunk=[&](uint16_t type,uint32_t blocks,uint32_t total){ size_t o=sparse.size(); sparse.resize(o+12,0); put16(sparse,o,type); put32(sparse,o+4,blocks); put32(sparse,o+8,total); };
  put32(sparse,0,0xED26FF3A); put16(sparse,4,1); put16(sparse,8,28); put16(sparse,10,12); put32(sparse,12,block); put32(sparse,16,4); put32(sparse,20,3);
  appendChunk(0xCAC1,2,12+block*2); sparse.resize(sparse.size()+block*2,0x41);
  appendChunk(0xCAC2,1,16); sparse.insert(sparse.end(),{1,2,3,4});
  appendChunk(0xCAC3,1,12);
  int fd=makeTemp(sparse); CHECK(fd>=0); if(fd<0)return;
  constexpr uint64_t ram=0x40000000ull, mmio=0x0A000000ull;
  Bus bus(1ull<<20); CHECK_ST(bus.addRam(GuestAddr(ram),64ull<<10),Status::Ok);
  std::unique_ptr<VirtioMmioBlock> dev; CHECK_ST(VirtioMmioBlock::openFd(&bus,fd,true,&dev),Status::Ok); close(fd);
  if(!dev)return;
  CHECK(dev->sparseImage()); CHECK_EQ(dev->capacitySectors(),4ull);
  bool irq=false; dev->setIrqSink([&](bool level){irq=level;});
  CHECK_ST(bus.addDevice(GuestAddr(mmio),VirtioMmioBlock::kRegionSize,dev.get()),Status::Ok);
  constexpr uint64_t desc=ram+0x1000, avail=ram+0x2000, used=ram+0x3000;
  std::vector<uint8_t> descriptors(48,0);
  put64(descriptors,0,ram+0x4000); put32(descriptors,8,16); put16(descriptors,12,1); put16(descriptors,14,1);
  put64(descriptors,16,ram+0x5000); put32(descriptors,24,2048); put16(descriptors,28,3); put16(descriptors,30,2);
  put64(descriptors,32,ram+0x6000); put32(descriptors,40,1); put16(descriptors,44,2);
  CHECK_ST(bus.loadBytes(GuestAddr(desc),descriptors.data(),descriptors.size()),Status::Ok);
  std::vector<uint8_t> request(16,0); put32(request,0,0); put64(request,8,0);
  CHECK_ST(bus.loadBytes(GuestAddr(ram+0x4000),request.data(),request.size()),Status::Ok);
  std::vector<uint8_t> availRing(8,0); put16(availRing,2,1); put16(availRing,4,0);
  CHECK_ST(bus.loadBytes(GuestAddr(avail),availRing.data(),availRing.size()),Status::Ok);
  std::vector<uint8_t> zeros(32,0); CHECK_ST(bus.loadBytes(GuestAddr(used),zeros.data(),zeros.size()),Status::Ok);
  auto wr=[&](uint64_t off,uint32_t v){return bus.write(GuestAddr(mmio+off),4,v);};
  CHECK_ST(wr(0x024,1),Status::Ok); CHECK_ST(wr(0x020,1),Status::Ok);
  CHECK_ST(wr(0x070,3),Status::Ok); CHECK_ST(wr(0x070,11),Status::Ok); CHECK_ST(wr(0x070,15),Status::Ok);
  CHECK_ST(wr(0x030,0),Status::Ok); CHECK_ST(wr(0x038,8),Status::Ok);
  CHECK_ST(wr(0x080,static_cast<uint32_t>(desc)),Status::Ok); CHECK_ST(wr(0x084,static_cast<uint32_t>(desc>>32)),Status::Ok);
  CHECK_ST(wr(0x090,static_cast<uint32_t>(avail)),Status::Ok); CHECK_ST(wr(0x094,static_cast<uint32_t>(avail>>32)),Status::Ok);
  CHECK_ST(wr(0x0a0,static_cast<uint32_t>(used)),Status::Ok); CHECK_ST(wr(0x0a4,static_cast<uint32_t>(used>>32)),Status::Ok);
  CHECK_ST(wr(0x044,1),Status::Ok); CHECK_ST(wr(0x050,0),Status::Ok); CHECK(irq);
  std::vector<uint8_t> out(2048,0xFF); CHECK_ST(bus.readBytes(GuestAddr(ram+0x5000),out.data(),out.size()),Status::Ok);
  CHECK_EQ(out[0],0x41ull); CHECK_EQ(out[1023],0x41ull);
  CHECK_EQ(out[1024],1ull); CHECK_EQ(out[1025],2ull); CHECK_EQ(out[1026],3ull); CHECK_EQ(out[1027],4ull);
  CHECK_EQ(out[1535],4ull); CHECK_EQ(out[1536],0ull); CHECK_EQ(out[2047],0ull);
  std::vector<uint8_t> status(1,0xFF); CHECK_ST(bus.readBytes(GuestAddr(ram+0x6000),status.data(),1),Status::Ok); CHECK_EQ(status[0],0ull);
}


NT_TEST(virtio_mmio_logical_partition_view_maps_linear_extents_without_copying_super) {
  // Backing disk has distinct 512-byte sectors. Logical view selects source sectors 2 and 5,
  // and a ZERO extent, exercising the same offset translation used by liblp dm-linear.
  std::vector<uint8_t> backing(8 * 512, 0);
  for (size_t sector = 0; sector < 8; ++sector)
    std::fill(backing.begin() + static_cast<std::ptrdiff_t>(sector * 512),
              backing.begin() + static_cast<std::ptrdiff_t>((sector + 1) * 512),
              static_cast<uint8_t>(0x10 + sector));
  int fd = makeTemp(backing); CHECK(fd >= 0); if (fd < 0) return;
  Bus bus(1ull << 20); CHECK_ST(bus.addRam(GuestAddr(0x40000000), 64ull << 10), Status::Ok);
  std::vector<LogicalBlockExtent> extents = {
      {0, 512, false, 0, 2 * 512ull},
      {512, 512, true, 0, 0},
      {1024, 512, false, 0, 5 * 512ull},
  };
  std::unique_ptr<VirtioMmioBlock> view;
  CHECK_ST(VirtioMmioBlock::openPartitionView(&bus, fd, 1536, extents, "system_a", &view), Status::Ok);
  close(fd);
  if (!view) return;
  CHECK(view->isPartitionView()); CHECK(view->readOnly()); CHECK_EQ(view->capacitySectors(), 3ull);
  uint64_t identity = 0;
  CHECK_ST(view->read(0x008, 4, &identity), Status::Ok); CHECK_EQ(identity, 2ull);
  std::vector<uint8_t> output(1536, 0xFF);
  CHECK_ST(view->readLogicalRange(0, output.data(), output.size()), Status::Ok);
  CHECK_EQ(output[0], 0x12ull); CHECK_EQ(output[511], 0x12ull);
  CHECK_EQ(output[512], 0ull); CHECK_EQ(output[1023], 0ull);
  CHECK_EQ(output[1024], 0x15ull); CHECK_EQ(output[1535], 0x15ull);
  CHECK_ST(view->readLogicalRange(1024, output.data(), 1024), Status::OutOfBounds);
  const std::string report = view->diagnostics();
  CHECK(report.find("label=system_a") != std::string::npos);
  CHECK(report.find("linear_view=true") != std::string::npos);
}

NT_TEST(virtio_mmio_logical_partition_view_rejects_gaps_and_external_backing_devices) {
  std::vector<uint8_t> backing(4096, 0x33);
  int fd = makeTemp(backing); CHECK(fd >= 0); if (fd < 0) return;
  Bus bus(1ull << 20); CHECK_ST(bus.addRam(GuestAddr(0x40000000), 64ull << 10), Status::Ok);
  std::unique_ptr<VirtioMmioBlock> view;
  std::vector<LogicalBlockExtent> gap = {{512, 512, false, 0, 0}};
  CHECK_ST(VirtioMmioBlock::openPartitionView(&bus, fd, 512, gap, "bad-gap", &view), Status::InvalidArgument);
  std::vector<LogicalBlockExtent> external = {{0, 512, false, 1, 0}};
  CHECK_ST(VirtioMmioBlock::openPartitionView(&bus, fd, 512, external, "external", &view), Status::InvalidArgument);
  close(fd);
}


NT_TEST(virtio_mmio_logical_partition_view_reads_expanded_sparse_super_offsets) {
  // Sparse backing: RAW sectors 0-1, FILL sector 2, DONT_CARE sector 3.
  const uint32_t block = 512;
  std::vector<uint8_t> sparse(28, 0);
  auto appendChunk=[&](uint16_t type,uint32_t blocks,uint32_t total){
    size_t o=sparse.size(); sparse.resize(o+12,0); put16(sparse,o,type); put32(sparse,o+4,blocks); put32(sparse,o+8,total);
  };
  put32(sparse,0,0xED26FF3A); put16(sparse,4,1); put16(sparse,8,28); put16(sparse,10,12);
  put32(sparse,12,block); put32(sparse,16,4); put32(sparse,20,3);
  appendChunk(0xCAC1,2,12+block*2); sparse.resize(sparse.size()+block*2,0x41);
  appendChunk(0xCAC2,1,16); sparse.insert(sparse.end(),{1,2,3,4});
  appendChunk(0xCAC3,1,12);
  int fd=makeTemp(sparse); CHECK(fd>=0); if(fd<0)return;
  Bus bus(1ull<<20); CHECK_ST(bus.addRam(GuestAddr(0x40000000),64ull<<10),Status::Ok);
  std::vector<LogicalBlockExtent> extents = {
      {0, 512, false, 0, 512},       // source RAW sector 1
      {512, 512, false, 0, 1024},    // source FILL sector
      {1024, 512, false, 0, 1536},   // source DONT_CARE sector -> zero fill
      {1536, 512, true, 0, 0},       // liblp ZERO extent
  };
  std::unique_ptr<VirtioMmioBlock> view;
  CHECK_ST(VirtioMmioBlock::openPartitionView(&bus,fd,2048,extents,"sparse-view",&view),Status::Ok);
  close(fd); if(!view)return;
  CHECK(view->sparseImage()); CHECK(view->isPartitionView()); CHECK_EQ(view->capacitySectors(),4ull);
  std::vector<uint8_t> output(2048,0xFF);
  CHECK_ST(view->readLogicalRange(0,output.data(),output.size()),Status::Ok);
  CHECK_EQ(output[0],0x41ull); CHECK_EQ(output[511],0x41ull);
  CHECK_EQ(output[512],1ull); CHECK_EQ(output[513],2ull); CHECK_EQ(output[514],3ull); CHECK_EQ(output[515],4ull);
  CHECK_EQ(output[1024],0ull); CHECK_EQ(output[1535],0ull); CHECK_EQ(output[1536],0ull); CHECK_EQ(output[2047],0ull);
}

NT_TEST(virtio_mmio_super_container_exposes_synthetic_gpt_super_partition) {
  std::vector<uint8_t> disk(4096, 0x5A);
  for (size_t i = 0; i < disk.size(); ++i) disk[i] = static_cast<uint8_t>((i * 7u + 3u) & 0xFFu);
  // Make sure this is a raw image, not the Android sparse magic.
  disk[0] = 0x45; disk[1] = 0x4C; disk[2] = 0x46; disk[3] = 0x21;
  int fd = makeTemp(disk); CHECK(fd >= 0); if (fd < 0) return;
  Bus bus(1ull << 20);
  std::unique_ptr<VirtioMmioBlock> dev;
  CHECK_ST(VirtioMmioBlock::openSuperContainerFd(&bus, fd, "super", &dev), Status::Ok);
  close(fd);
  if (!dev) return;
  CHECK(dev->isSuperContainer());
  CHECK(dev->readOnly());
  CHECK_EQ(dev->capacitySectors(), 2048ull + 8ull + 34ull);
  CHECK(dev->diagnostics().find("synthetic_gpt_super=true") != std::string::npos);

  std::array<uint8_t, 512> sector{};
  CHECK_ST(dev->readLogicalRange(0, sector.data(), sector.size()), Status::Ok);
  CHECK_EQ(sector[510], 0x55ull); CHECK_EQ(sector[511], 0xAAull);
  CHECK_EQ(sector[446 + 4], 0xEEull); // protective MBR
  CHECK_EQ(sector[446 + 8], 1ull);    // protective partition starts at LBA 1

  CHECK_ST(dev->readLogicalRange(512, sector.data(), sector.size()), Status::Ok);
  CHECK(std::memcmp(sector.data(), "EFI PART", 8) == 0);
  CHECK_EQ(static_cast<uint64_t>(sector[12]), 92ull); // GPT header size low byte
  const uint64_t totalSectors = 2048ull + 8ull + 34ull;
  CHECK_EQ(static_cast<uint64_t>(sector[24]), 1ull); // current LBA low byte
  CHECK_EQ(get64(sector.data() + 32), totalSectors - 1ull); // backup LBA
  const uint32_t storedHeaderCrc = static_cast<uint32_t>(sector[16]) |
      (static_cast<uint32_t>(sector[17]) << 8) | (static_cast<uint32_t>(sector[18]) << 16) |
      (static_cast<uint32_t>(sector[19]) << 24);
  auto crc32 = [](const uint8_t* data, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
      crc ^= data[i];
      for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
  };
  sector[16] = sector[17] = sector[18] = sector[19] = 0;
  CHECK_EQ(storedHeaderCrc, crc32(sector.data(), 92));
  const uint32_t storedEntriesCrc = static_cast<uint32_t>(sector[88]) |
      (static_cast<uint32_t>(sector[89]) << 8) | (static_cast<uint32_t>(sector[90]) << 16) |
      (static_cast<uint32_t>(sector[91]) << 24);

  std::vector<uint8_t> entries(128u * 128u, 0);
  CHECK_ST(dev->readLogicalRange(2ull * 512ull, entries.data(), entries.size()), Status::Ok);
  CHECK_EQ(storedEntriesCrc, crc32(entries.data(), entries.size()));
  CHECK(std::memcmp(entries.data() + 56, "s\0u\0p\0e\0r\0", 10) == 0);
  CHECK_EQ(get64(entries.data() + 32), 2048ull); // first LBA
  CHECK_EQ(get64(entries.data() + 40), 2055ull); // last LBA

  std::array<uint8_t, 64> payload{};
  CHECK_ST(dev->readLogicalRange(2048ull * 512ull, payload.data(), payload.size()), Status::Ok);
  CHECK(std::memcmp(payload.data(), disk.data(), payload.size()) == 0);
  CHECK_ST(dev->readLogicalRange((totalSectors - 1ull) * 512ull, sector.data(), sector.size()), Status::Ok);
  CHECK(std::memcmp(sector.data(), "EFI PART", 8) == 0);
  const uint32_t storedBackupCrc = static_cast<uint32_t>(sector[16]) |
      (static_cast<uint32_t>(sector[17]) << 8) | (static_cast<uint32_t>(sector[18]) << 16) |
      (static_cast<uint32_t>(sector[19]) << 24);
  sector[16] = sector[17] = sector[18] = sector[19] = 0;
  CHECK_EQ(storedBackupCrc, crc32(sector.data(), 92));
}

NT_TEST(virtio_mmio_super_container_expands_sparse_super_bytes_inside_gpt_partition) {
  const uint32_t block = 512;
  std::vector<uint8_t> sparse(28, 0);
  put32(sparse, 0, 0xED26FF3A); put16(sparse, 4, 1); put16(sparse, 8, 28); put16(sparse, 10, 12);
  put32(sparse, 12, block); put32(sparse, 16, 4); put32(sparse, 20, 3);
  auto appendChunk = [&](uint16_t type, uint32_t blocks, uint32_t totalSize) {
    const size_t o = sparse.size(); sparse.resize(o + 12, 0); put16(sparse, o, type);
    put32(sparse, o + 4, blocks); put32(sparse, o + 8, totalSize);
  };
  appendChunk(0xCAC1, 2, 12 + block * 2); sparse.resize(sparse.size() + block * 2, 0x41);
  appendChunk(0xCAC2, 1, 16); sparse.insert(sparse.end(), {1,2,3,4});
  appendChunk(0xCAC3, 1, 12);
  int fd = makeTemp(sparse); CHECK(fd >= 0); if (fd < 0) return;
  Bus bus(1ull << 20);
  std::unique_ptr<VirtioMmioBlock> dev;
  CHECK_ST(VirtioMmioBlock::openSuperContainerFd(&bus, fd, "sparse-super", &dev), Status::Ok);
  close(fd);
  if (!dev) return;
  CHECK(dev->sparseImage()); CHECK(dev->isSuperContainer());
  std::vector<uint8_t> expanded(2048, 0xFF);
  CHECK_ST(dev->readLogicalRange(2048ull * 512ull, expanded.data(), expanded.size()), Status::Ok);
  CHECK_EQ(expanded[0], 0x41ull); CHECK_EQ(expanded[1023], 0x41ull);
  CHECK_EQ(expanded[1024], 1ull); CHECK_EQ(expanded[1025], 2ull);
  CHECK_EQ(expanded[1026], 3ull); CHECK_EQ(expanded[1027], 4ull);
  CHECK_EQ(expanded[1536], 0ull); CHECK_EQ(expanded[2047], 0ull);
}
