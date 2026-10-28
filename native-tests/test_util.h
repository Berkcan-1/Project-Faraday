#pragma once
#include "cpu/ncv_cpu.h"
#include "devices/ncv_uart.h"
#include "devices/ncv_gicv2.h"
#include "memory/ncv_bus.h"
#include "test_framework.h"

// Bus + UART + CPU'dan oluşan minimal test makinesi (Vm'den bağımsız).
struct Machine {
  static constexpr uint64_t kBase = 0x40000000ull, kRam = 0x10000ull, kUart = 0x09000000ull;
  ncv::Bus bus;
  ncv::Pl011Uart uart;
  ncv::GicV2 gic;
  ncv::Cpu cpu;
  Machine() : cpu(&bus, &gic) {
    bus.addRam(ncv::GuestAddr(kBase), kRam);
    bus.addDevice(ncv::GuestAddr(kUart), ncv::Pl011Uart::kRegionSize, &uart);
    bus.addDevice(ncv::GuestAddr(0x08000000ull), ncv::GicV2::kRegionSize, &gic);
  }
  void load(const uint8_t* p, size_t n) {
    CHECK_ST(bus.loadBytes(ncv::GuestAddr(kBase), p, n), ncv::Status::Ok);
    cpu.reset(kBase, kBase + kRam);
  }
  ncv::StopReason run(uint64_t max = 100000) { return cpu.run(max); }
  uint64_t x(int i) const { return cpu.state().x[i]; }
  void setX(int i, uint64_t v) { cpu.mutableState().x[i] = v; }
  uint64_t rd(uint64_t addr, unsigned size) {
    uint64_t v = ~0ull;
    CHECK_ST(bus.read(ncv::GuestAddr(addr), size, &v), ncv::Status::Ok);
    return v;
  }
};
