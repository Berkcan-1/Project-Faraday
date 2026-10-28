#include <cstdio>
#include <string>

#include "loader/ncv_loader.h"
#include "test_framework.h"

using namespace ncv;

namespace {
std::string writeTemp(const char* name, size_t n) {
  std::string path = std::string("/tmp/") + name;
  FILE* f = std::fopen(path.c_str(), "wb");
  for (size_t i = 0; i < n; i++) std::fputc((int)(i & 0xFF), f);
  std::fclose(f);
  return path;
}
}  // namespace

NT_TEST(loader_loads_file_into_ram) {
  Bus bus;
  bus.addRam(GuestAddr(0x1000), 0x1000);
  const std::string p = writeTemp("ncv_t1.bin", 300);
  uint64_t n = 0;
  CHECK_ST(loadRawFile(bus, GuestAddr(0x1010), p.c_str(), 1 << 20, &n), Status::Ok);
  CHECK_EQ(n, 300ull);
  uint64_t v = 0;
  bus.read(GuestAddr(0x1010 + 299), 1, &v);
  CHECK_EQ(v, (uint64_t)(299 & 0xFF));
  std::remove(p.c_str());
}

NT_TEST(loader_rejects_bad_inputs) {
  Bus bus;
  bus.addRam(GuestAddr(0x1000), 0x100);
  uint64_t n = 99;
  CHECK_ST(loadRawFile(bus, GuestAddr(0x1000), "/tmp/ncv_does_not_exist.bin", 1024, &n), Status::IoError);
  CHECK_EQ(n, 0ull);
  CHECK_ST(loadRawFile(bus, GuestAddr(0x1000), nullptr, 1024, &n), Status::InvalidArgument);
  const std::string big = writeTemp("ncv_t2.bin", 5000);
  CHECK_ST(loadRawFile(bus, GuestAddr(0x1000), big.c_str(), 1024, &n), Status::InvalidArgument);   // dosya limiti aşıyor
  CHECK_ST(loadRawFile(bus, GuestAddr(0x1000), big.c_str(), 1 << 20, &n), Status::OutOfBounds);    // RAM'e sığmıyor
  CHECK_ST(loadRawFile(bus, GuestAddr(0x9000), big.c_str(), 1 << 20, &n), Status::Unmapped);
  std::remove(big.c_str());
}
