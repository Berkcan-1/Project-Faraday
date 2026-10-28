#include "memory/ncv_bus.h"
#include "test_framework.h"
#include <array>

using namespace ncv;

namespace {
struct RecordingDevice : Device {
  uint64_t lastOffset = ~0ull, lastValue = 0; unsigned lastSize = 0; int reads = 0, writes = 0; Status ret = Status::Ok;
  const char* name() const override { return "rec"; }
  Status read(uint64_t o, unsigned s, uint64_t* v) override { lastOffset = o; lastSize = s; reads++; *v = 0x1122334455667788ull; return ret; }
  Status write(uint64_t o, unsigned s, uint64_t v) override { lastOffset = o; lastSize = s; lastValue = v; writes++; return ret; }
};
}  // namespace

NT_TEST(bus_ram_roundtrip_little_endian) {
  Bus bus;
  CHECK_ST(bus.addRam(GuestAddr(0x1000), 0x100), Status::Ok);
  CHECK_ST(bus.write(GuestAddr(0x1000), 8, 0x0102030405060708ull), Status::Ok);
  uint64_t v = 0;
  CHECK_ST(bus.read(GuestAddr(0x1000), 8, &v), Status::Ok);
  CHECK_EQ(v, 0x0102030405060708ull);
  CHECK_ST(bus.read(GuestAddr(0x1000), 1, &v), Status::Ok);
  CHECK_EQ(v, 0x08ull);                 // en düşük bayt en düşük adreste
  CHECK_ST(bus.read(GuestAddr(0x1007), 1, &v), Status::Ok);
  CHECK_EQ(v, 0x01ull);
  CHECK_ST(bus.read(GuestAddr(0x1002), 2, &v), Status::Ok);
  CHECK_EQ(v, 0x0506ull);
  CHECK_ST(bus.read(GuestAddr(0x1004), 4, &v), Status::Ok);
  CHECK_EQ(v, 0x01020304ull);
}

NT_TEST(bus_write_truncates_to_access_size) {
  Bus bus;
  bus.addRam(GuestAddr(0), 16);
  bus.write(GuestAddr(0), 8, ~0ull);
  CHECK_ST(bus.write(GuestAddr(0), 2, 0xAABBCCDDull), Status::Ok);  // yalnızca düşük 16 bit yazılmalı
  uint64_t v = 0;
  bus.read(GuestAddr(0), 8, &v);
  CHECK_EQ(v, 0xFFFFFFFFFFFFCCDDull);
}

NT_TEST(bus_bounds_and_unmapped) {
  Bus bus;
  bus.addRam(GuestAddr(0x1000), 0x100);
  uint64_t v = 0;
  CHECK_ST(bus.read(GuestAddr(0xFFF), 1, &v), Status::Unmapped);
  CHECK_ST(bus.read(GuestAddr(0x1100), 1, &v), Status::Unmapped);
  CHECK_ST(bus.read(GuestAddr(0x10FF), 1, &v), Status::Ok);
  CHECK_ST(bus.read(GuestAddr(0x10FC), 8, &v), Status::OutOfBounds);  // bölge sonunu aşıyor
  CHECK_ST(bus.write(GuestAddr(0x10FC), 8, 0), Status::OutOfBounds);
  CHECK_ST(bus.read(GuestAddr(~0ull), 8, &v), Status::Unmapped);      // adres taşması da güvenli
}

NT_TEST(bus_invalid_size_and_null) {
  Bus bus;
  bus.addRam(GuestAddr(0), 16);
  uint64_t v = 0;
  CHECK_ST(bus.read(GuestAddr(0), 3, &v), Status::InvalidArgument);
  CHECK_ST(bus.read(GuestAddr(0), 0, &v), Status::InvalidArgument);
  CHECK_ST(bus.write(GuestAddr(0), 16, 0), Status::InvalidArgument);
  CHECK_ST(bus.read(GuestAddr(0), 4, nullptr), Status::InvalidArgument);
}

NT_TEST(bus_region_validation) {
  Bus bus(1 << 20);
  CHECK_ST(bus.addRam(GuestAddr(0x1000), 0), Status::InvalidArgument);
  CHECK_ST(bus.addRam(GuestAddr(~0ull - 5), 0x100), Status::InvalidArgument);       // 64-bit taşma
  CHECK_ST(bus.addRam(GuestAddr(0x1000), 0x1000), Status::Ok);
  CHECK_ST(bus.addRam(GuestAddr(0x1800), 0x1000), Status::Overlap);
  CHECK_ST(bus.addRam(GuestAddr(0x0800), 0x1000), Status::Overlap);
  CHECK_ST(bus.addRam(GuestAddr(0x1000), 0x1000), Status::Overlap);
  CHECK_ST(bus.addRam(GuestAddr(0x2000), 0x1000), Status::Ok);                      // bitişik olmak çakışma değil
  CHECK_ST(bus.addRam(GuestAddr(0x0000), 0x1000), Status::Ok);
  CHECK_EQ(bus.regionCount(), 3u);
  RecordingDevice d;
  CHECK_ST(bus.addDevice(GuestAddr(0x1800), 0x10, &d), Status::Overlap);
  CHECK_ST(bus.addDevice(GuestAddr(0x9000), 0x10, nullptr), Status::InvalidArgument);
}

NT_TEST(bus_ram_limit) {
  Bus bus(0x2000);
  CHECK_ST(bus.addRam(GuestAddr(0), 0x1800), Status::Ok);
  CHECK_ST(bus.addRam(GuestAddr(0x10000), 0x1000), Status::OutOfMemory);  // toplam limit aşılıyor
  CHECK_ST(bus.addRam(GuestAddr(0x10000), 0x800), Status::Ok);
  CHECK_EQ(bus.totalRamBytes(), 0x2000ull);
}

NT_TEST(bus_adjacent_regions_no_spanning_access) {
  Bus bus;
  bus.addRam(GuestAddr(0), 0x10);
  bus.addRam(GuestAddr(0x10), 0x10);
  uint64_t v = 0;
  CHECK_ST(bus.read(GuestAddr(0xC), 8, &v), Status::OutOfBounds);  // iki bölgeye taşan erişim reddedilir
  CHECK_ST(bus.read(GuestAddr(0x10), 8, &v), Status::Ok);
}

NT_TEST(bus_mmio_routing) {
  Bus bus;
  RecordingDevice d;
  CHECK_ST(bus.addDevice(GuestAddr(0x9000000), 0x1000, &d), Status::Ok);
  uint64_t v = 0;
  CHECK_ST(bus.read(GuestAddr(0x9000018), 4, &v), Status::Ok);
  CHECK_EQ(d.lastOffset, 0x18ull);   // aygıt bölge-göreli ofset görür
  CHECK_EQ(d.lastSize, 4u);
  CHECK_EQ(v, 0x55667788ull);        // Bus, okunan değeri erişim boyutuna maskeler
  CHECK_ST(bus.write(GuestAddr(0x9000000), 1, 0x1FFull), Status::Ok);
  CHECK_EQ(d.lastValue, 0xFFull);
  CHECK_EQ(d.writes, 1);
  d.ret = Status::DeviceError;
  CHECK_ST(bus.read(GuestAddr(0x9000000), 4, &v), Status::DeviceError);   // aygıt hatası yayılır
  CHECK_ST(bus.write(GuestAddr(0x9000000), 4, 0), Status::DeviceError);
  CHECK_ST(bus.read(GuestAddr(0x9000FFC), 8, &v), Status::OutOfBounds);   // aygıt penceresini aşan erişim aygıta gitmez
  d.reads = 0;
  bus.read(GuestAddr(0x9000FFC), 8, &v);
  CHECK_EQ(d.reads, 0);
}

NT_TEST(bus_load_and_read_bytes) {
  Bus bus;
  bus.addRam(GuestAddr(0x100), 8);
  RecordingDevice d;
  bus.addDevice(GuestAddr(0x200), 8, &d);
  const uint8_t src[4] = {1, 2, 3, 4};
  uint8_t out[4] = {};
  CHECK_ST(bus.loadBytes(GuestAddr(0x102), src, 4), Status::Ok);
  CHECK_ST(bus.readBytes(GuestAddr(0x102), out, 4), Status::Ok);
  CHECK(out[0] == 1 && out[3] == 4);
  CHECK_ST(bus.loadBytes(GuestAddr(0x105), src, 4), Status::OutOfBounds);
  CHECK_ST(bus.loadBytes(GuestAddr(0x200), src, 4), Status::InvalidArgument);   // MMIO'ya yükleme yok
  CHECK_ST(bus.loadBytes(GuestAddr(0x300), src, 4), Status::Unmapped);
  CHECK_ST(bus.loadBytes(GuestAddr(0x100), nullptr, 4), Status::InvalidArgument);
  CHECK_ST(bus.loadBytes(GuestAddr(0x100), src, ~0ull), Status::OutOfBounds);   // uzunluk taşması
  CHECK_ST(bus.loadBytes(GuestAddr(0x300), nullptr, 0), Status::Ok);            // 0 bayt no-op
  bus.zeroRam();
  CHECK_ST(bus.readBytes(GuestAddr(0x102), out, 4), Status::Ok);
  CHECK(out[0] == 0 && out[3] == 0);
}


NT_TEST(bus_lazy_ram_can_expose_two_gib_without_eager_allocation) {
  constexpr uint64_t kTwoGiB = 2ull << 30;
  Bus bus(kTwoGiB);
  CHECK_ST(bus.addRam(GuestAddr(0x40000000ull), kTwoGiB), Status::Ok);
  CHECK_EQ(bus.totalRamBytes(), kTwoGiB);
  CHECK_EQ(bus.residentRamBytes(), 0ull);  // mapping capacity does not eagerly allocate host RAM
  uint64_t value = 0xFFFFFFFFFFFFFFFFull;
  CHECK_ST(bus.read(GuestAddr(0x40000000ull + kTwoGiB - 1), 1, &value), Status::Ok);
  CHECK_EQ(value, 0ull);  // uncommitted pages read as zero
  CHECK_ST(bus.write(GuestAddr(0x40000000ull + kTwoGiB - 1), 1, 0xA5), Status::Ok);
  CHECK_ST(bus.read(GuestAddr(0x40000000ull + kTwoGiB - 1), 1, &value), Status::Ok);
  CHECK_EQ(value, 0xA5ull);
  CHECK_EQ(bus.residentRamBytes(), Bus::kRamPageSize);
  bus.zeroRam();
  CHECK_ST(bus.read(GuestAddr(0x40000000ull + kTwoGiB - 1), 1, &value), Status::Ok);
  CHECK_EQ(value, 0ull);
  CHECK_EQ(bus.residentRamBytes(), 0ull);
}

NT_TEST(bus_lazy_ram_cross_page_access_and_zero_write) {
  Bus bus(2ull << 30);
  CHECK_ST(bus.addRam(GuestAddr(0), 2ull << 30), Status::Ok);
  CHECK_ST(bus.write(GuestAddr(Bus::kRamPageSize - 3), 8, 0x8877665544332211ull), Status::Ok);
  uint64_t value = 0;
  CHECK_ST(bus.read(GuestAddr(Bus::kRamPageSize - 3), 8, &value), Status::Ok);
  CHECK_EQ(value, 0x8877665544332211ull);
  CHECK_EQ(bus.residentRamBytes(), 2 * Bus::kRamPageSize);
  CHECK_ST(bus.write(GuestAddr(Bus::kRamPageSize * 3), 8, 0), Status::Ok);
  CHECK_EQ(bus.residentRamBytes(), 2 * Bus::kRamPageSize);  // all-zero stores do not commit new pages
}

NT_TEST(bus_lazy_load_bytes_zero_pages_remain_unbacked) {
  Bus bus(2ull << 30);
  CHECK_ST(bus.addRam(GuestAddr(0x40000000ull), 2ull << 30), Status::Ok);
  std::array<uint8_t, 8192> zeroes{};
  CHECK_ST(bus.loadBytes(GuestAddr(0x40001000ull), zeroes.data(), zeroes.size()), Status::Ok);
  CHECK_EQ(bus.residentRamBytes(), 0ull);
  zeroes[5000] = 0x42;
  CHECK_ST(bus.loadBytes(GuestAddr(0x40001000ull), zeroes.data(), zeroes.size()), Status::Ok);
  CHECK_EQ(bus.residentRamBytes(), Bus::kRamPageSize);
  std::array<uint8_t, 8192> output{};
  CHECK_ST(bus.readBytes(GuestAddr(0x40001000ull), output.data(), output.size()), Status::Ok);
  CHECK(output[5000] == 0x42 && output[0] == 0 && output[8191] == 0);
}
