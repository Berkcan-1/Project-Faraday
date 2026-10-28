#include <cstring>
#include <algorithm>
#include <vector>
#include <zlib.h>
#include "core/ncv_vm.h"
#include "loader/ncv_dtb.h"
#include "test_util.h"

using namespace ncv;

namespace {
uint32_t be32(const std::vector<uint8_t>& b, size_t o) {
  return (static_cast<uint32_t>(b[o]) << 24) | (static_cast<uint32_t>(b[o + 1]) << 16) |
         (static_cast<uint32_t>(b[o + 2]) << 8) | static_cast<uint32_t>(b[o + 3]);
}
bool hasString(const std::vector<uint8_t>& b, const char* s) {
  const auto n = std::strlen(s);
  return std::search(b.begin(), b.end(), s, s + n) != b.end();
}
}

NT_TEST(stage4c_gicv2_register_flow_and_level_repend) {
  GicV2 g;
  uint64_t v = 0;
  CHECK_ST(g.read(0x004, 4, &v), Status::Ok); CHECK_EQ(v, 7ull);
  CHECK_ST(g.write(0x000, 4, 1), Status::Ok);       // GICD_CTLR
  CHECK_ST(g.write(0x10000, 4, 1), Status::Ok);    // GICC_CTLR
  CHECK_ST(g.write(0x10004, 4, 0xFF), Status::Ok); // PMR
  // Touch the final implemented words to guard against register-index/array overruns.
  CHECK_ST(g.write(0x43C, 4, 0x40302010u), Status::Ok); // priorities for IRQs 60..63
  CHECK_ST(g.read(0x43C, 4, &v), Status::Ok); CHECK_EQ(v, 0x40302010ull);
  CHECK_ST(g.write(0x83C, 4, 0x01010101u), Status::Ok); // target IDs 60..63 -> CPU 0
  CHECK_ST(g.read(0x83C, 4, &v), Status::Ok); CHECK_EQ(v, 0x01010101ull);
  CHECK_ST(g.write(0xC0C, 4, 0), Status::Ok); // last supported ICFGR word
  CHECK_ST(g.read(0xC0C, 4, &v), Status::Ok); CHECK_EQ(v, 0ull);
  CHECK_ST(g.write(0x100, 4, 1u << 27), Status::Ok); // enable virtual timer PPI
  g.setLevel(27, true);
  CHECK(g.hasDeliverableInterrupt());
  CHECK_ST(g.read(0x1000C, 4, &v), Status::Ok); CHECK_EQ(v, 27ull); // IAR acknowledges
  CHECK(!g.hasDeliverableInterrupt());
  CHECK_ST(g.write(0x10010, 4, 27), Status::Ok); // EOI; still-high level re-pends
  CHECK(g.hasDeliverableInterrupt());
}

NT_TEST(stage4c_pl011_rx_irq_reaches_gic_and_clears_after_read) {
  Pl011Uart uart;
  GicV2 gic;
  uart.setIrqSink([&gic](bool asserted) { gic.setLevel(33, asserted); });

  CHECK_ST(gic.write(0x000, 4, 1), Status::Ok);       // distributor enable
  CHECK_ST(gic.write(0x10000, 4, 1), Status::Ok);     // CPU interface enable
  CHECK_ST(gic.write(0x10004, 4, 0xFF), Status::Ok);  // priority mask
  CHECK_ST(gic.write(0x104, 4, 1u << 1), Status::Ok); // enable SPI 1 / GIC ID 33
  CHECK_ST(uart.write(0x038, 4, 1u << 4), Status::Ok); // RX interrupt mask
  CHECK(uart.pushRx(static_cast<uint8_t>('A')));
  CHECK(gic.hasDeliverableInterrupt());

  uint64_t v = 0;
  CHECK_ST(gic.read(0x1000C, 4, &v), Status::Ok); CHECK_EQ(v, 33ull); // IAR
  CHECK_ST(uart.read(0x000, 4, &v), Status::Ok); CHECK_EQ(v, static_cast<uint64_t>('A'));
  CHECK_ST(gic.write(0x10010, 4, 33), Status::Ok); // EOI after RX condition clears
  CHECK(!gic.hasDeliverableInterrupt());
}

NT_TEST(stage4c_virtual_timer_requests_irq_at_instruction_boundary) {
  Machine m;
  const uint32_t nop = 0xD503201Fu;
  const uint8_t insn[4] = {static_cast<uint8_t>(nop), static_cast<uint8_t>(nop >> 8),
                           static_cast<uint8_t>(nop >> 16), static_cast<uint8_t>(nop >> 24)};
  CHECK_ST(m.bus.loadBytes(GuestAddr(Machine::kBase), insn, sizeof(insn)), Status::Ok);
  m.cpu.reset(Machine::kBase, Machine::kBase + Machine::kRam, 1);
  CpuState& s = m.cpu.mutableState();
  s.vbar_el1 = Machine::kBase;
  s.daif = 0; // unmask IRQ
  s.cntv_cval_el0 = 0;
  s.cntv_ctl_el0 = 1; // enabled, unmasked, already expired
  CHECK_ST(m.bus.write(GuestAddr(0x08000000ull), 4, 1), Status::Ok); // GICD_CTLR
  CHECK_ST(m.bus.write(GuestAddr(0x08000000ull + 0x100), 4, 1u << 27), Status::Ok);
  CHECK_ST(m.bus.write(GuestAddr(0x08010000ull), 4, 1), Status::Ok); // GICC_CTLR
  CHECK_ST(m.bus.write(GuestAddr(0x08010000ull + 0x004), 4, 0xFF), Status::Ok);
  CHECK(m.cpu.step() == StopReason::None);
  CHECK_EQ(s.current_el, 1u);
  CHECK_EQ(s.pc, Machine::kBase + 0x280ull); // same-EL SPx IRQ vector
  CHECK_EQ(s.elr_el1, Machine::kBase);
  CHECK((s.daif & 2u) != 0);
}

NT_TEST(stage4d_linux_dtb_header_and_board_nodes) {
  LinuxDtbConfig config;
  config.ramBase = 0x40000000ull;
  config.ramSize = 256ull << 20;
  config.bootargs = "console=ttyAMA0";
  std::vector<uint8_t> dtb;
  CHECK_ST(buildLinuxDtb(config, &dtb), Status::Ok);
  CHECK(dtb.size() >= 56);
  CHECK_EQ(be32(dtb, 0), 0xD00Dfeedull);
  CHECK_EQ(be32(dtb, 4), dtb.size());
  CHECK_EQ(be32(dtb, 16), 40ull); // reserve map offset
  CHECK_EQ(be32(dtb, 20), 17ull); // FDT version
  CHECK(be32(dtb, 8) < dtb.size());
  CHECK(be32(dtb, 12) < dtb.size());
  CHECK(hasString(dtb, "arm,armv8-timer"));
  CHECK(hasString(dtb, "linux,dummy-virt"));
  CHECK(hasString(dtb, "arm,cortex-a15-gic"));
  CHECK(hasString(dtb, "arm,pl011"));
  CHECK(hasString(dtb, "arm,pl031"));
  CHECK(hasString(dtb, "virtio_rng@a002000"));
  CHECK(hasString(dtb, "simple-framebuffer"));
  CHECK(hasString(dtb, "arm,psci-1.0"));
  CHECK(hasString(dtb, "console=ttyAMA0"));
  CHECK_ST(buildLinuxDtb(config, nullptr), Status::InvalidArgument);
}

NT_TEST(stage4d_vm_prepares_dtb_and_passes_it_in_x0) {
  BoardConfig cfg;
  cfg.ramSize = 16ull << 20;
  Status status;
  auto vm = Vm::create(cfg, &status);
  CHECK_ST(status, Status::Ok);
  if (!vm) return;
  const uint32_t hlt = 0xD4400000u;
  const uint8_t code[4] = {static_cast<uint8_t>(hlt), static_cast<uint8_t>(hlt >> 8),
                           static_cast<uint8_t>(hlt >> 16), static_cast<uint8_t>(hlt >> 24)};
  CHECK_ST(vm->loadPayload(GuestAddr(cfg.ramBase), code, sizeof(code)), Status::Ok);
  CHECK_ST(vm->setEntry(GuestAddr(cfg.ramBase)), Status::Ok);
  GuestAddr dtbAddr;
  CHECK_ST(vm->prepareLinuxBootDtb("console=ttyAMA0", &dtbAddr), Status::Ok);
  CHECK(dtbAddr.value >= cfg.ramBase && dtbAddr.value < cfg.ramBase + cfg.ramSize);
  CHECK(vm->runSync(4) == StopReason::Halted);
  CpuState cpu;
  CHECK_ST(vm->cpuState(&cpu), Status::Ok);
  CHECK_EQ(cpu.current_el, 1u);
  CHECK_EQ(cpu.x[0], dtbAddr.value);
  uint64_t magic = 0;
  CHECK_ST(vm->bus().read(dtbAddr, 4, &magic), Status::Ok);
  CHECK_EQ(magic, 0xEDFE0DD0ull); // FDT magic as little-endian guest memory word
}

NT_TEST(stage4d_feature_id_and_counter_registers_are_readable_at_el1) {
  Machine m;
  const uint32_t program[] = {0xD5380400u, 0xD4400000u}; // MRS X0, ID_AA64PFR0_EL1; HLT
  uint8_t bytes[8];
  for (unsigned i = 0; i < 2; ++i) {
    bytes[i * 4] = static_cast<uint8_t>(program[i]);
    bytes[i * 4 + 1] = static_cast<uint8_t>(program[i] >> 8);
    bytes[i * 4 + 2] = static_cast<uint8_t>(program[i] >> 16);
    bytes[i * 4 + 3] = static_cast<uint8_t>(program[i] >> 24);
  }
  CHECK_ST(m.bus.loadBytes(GuestAddr(Machine::kBase), bytes, sizeof(bytes)), Status::Ok);
  m.cpu.reset(Machine::kBase, Machine::kBase + Machine::kRam, 1);
  CHECK(m.run(8) == StopReason::Halted);
  CHECK_EQ(m.cpu.state().x[0], 0x11ull);

  const uint32_t timerProgram[] = {0xD53BE000u, 0xD4400000u}; // MRS X0, CNTFRQ_EL0; HLT
  for (unsigned i = 0; i < 2; ++i) {
    bytes[i * 4] = static_cast<uint8_t>(timerProgram[i]);
    bytes[i * 4 + 1] = static_cast<uint8_t>(timerProgram[i] >> 8);
    bytes[i * 4 + 2] = static_cast<uint8_t>(timerProgram[i] >> 16);
    bytes[i * 4 + 3] = static_cast<uint8_t>(timerProgram[i] >> 24);
  }
  CHECK_ST(m.bus.loadBytes(GuestAddr(Machine::kBase), bytes, sizeof(bytes)), Status::Ok);
  m.cpu.reset(Machine::kBase, Machine::kBase + Machine::kRam, 1);
  CHECK(m.run(8) == StopReason::Halted);
  CHECK_EQ(m.cpu.state().x[0], 1000000ull);
}


namespace {
void putLe32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
  b[o] = static_cast<uint8_t>(v); b[o + 1] = static_cast<uint8_t>(v >> 8);
  b[o + 2] = static_cast<uint8_t>(v >> 16); b[o + 3] = static_cast<uint8_t>(v >> 24);
}
void putLe64(std::vector<uint8_t>& b, size_t o, uint64_t v) {
  putLe32(b, o, static_cast<uint32_t>(v));
  putLe32(b, o + 4, static_cast<uint32_t>(v >> 32));
}
std::vector<uint8_t> gzipBytes(const std::vector<uint8_t>& input) {
  z_stream stream{};
  if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, MAX_WBITS + 16, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK) return {};
  std::vector<uint8_t> output(input.size() + 1024);
  stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  stream.next_out = reinterpret_cast<Bytef*>(output.data());
  stream.avail_out = static_cast<uInt>(output.size());
  const int result = deflate(&stream, Z_FINISH);
  const size_t size = static_cast<size_t>(stream.total_out);
  deflateEnd(&stream);
  if (result != Z_STREAM_END) return {};
  output.resize(size);
  return output;
}
}

NT_TEST(stage4d_loads_raw_arm64_linux_image_and_prepares_direct_boot) {
  BoardConfig cfg;
  cfg.ramSize = 16ull << 20;
  Status status;
  auto vm = Vm::create(cfg, &status);
  CHECK_ST(status, Status::Ok);
  if (!vm) return;

  // Minimal synthetic, uncompressed ARM64 Image header. The first instruction is HLT
  // so this test verifies placement/entry/DTB handoff, not an actual Linux boot.
  std::vector<uint8_t> image(64, 0);
  putLe32(image, 0, 0xD4400000u);
  putLe32(image, 4, 0xD4400000u);
  putLe64(image, 8, 0x80000ull);  // text_offset
  putLe64(image, 16, 0x1000ull);  // image_size
  putLe64(image, 24, 0x2ull);     // 4 KiB kernel page-size profile
  putLe32(image, 56, 0x644D5241u);

  GuestAddr entry;
  CHECK_ST(vm->loadLinuxImage(image.data(), image.size(), &entry), Status::Ok);
  CHECK_EQ(entry.value, cfg.ramBase + 0x80000ull);
  GuestAddr dtb;
  CHECK_ST(vm->prepareLinuxBootDtb("console=ttyAMA0", &dtb), Status::Ok);
  CHECK(vm->runSync(4) == StopReason::Halted);
  CpuState state;
  CHECK_ST(vm->cpuState(&state), Status::Ok);
  CHECK_EQ(state.current_el, 1u);
  CHECK_EQ(state.pc, entry.value);
  CHECK_EQ(state.x[0], dtb.value);
  CHECK_EQ(state.x[1], 0ull);
  CHECK_EQ(state.x[2], 0ull);
  CHECK_EQ(state.x[3], 0ull);
  CHECK_EQ(state.daif, 0xFu);
  CHECK_EQ(state.sctlr_el1, 0x30D00800ull); // RES1 bits set; MMU and caches off at Linux entry.
}

NT_TEST(stage4d_loads_gzip_compressed_arm64_linux_image) {
  BoardConfig cfg;
  cfg.ramSize = 16ull << 20;
  Status status;
  auto vm = Vm::create(cfg, &status);
  CHECK_ST(status, Status::Ok);
  if (!vm) return;

  std::vector<uint8_t> image(64, 0);
  putLe32(image, 0, 0xD4400000u); // synthetic HLT, not a real Linux kernel
  putLe64(image, 8, 0x80000ull);
  putLe64(image, 16, 0x1000ull);
  putLe64(image, 24, 0x2ull);
  putLe32(image, 56, 0x644D5241u);
  const auto compressed = gzipBytes(image);
  CHECK(!compressed.empty());
  GuestAddr entry;
  CHECK_ST(vm->loadLinuxImage(compressed.data(), compressed.size(), &entry), Status::Ok);
  CHECK_EQ(entry.value, cfg.ramBase + 0x80000ull);
  CHECK_ST(vm->prepareLinuxBootDtb("console=ttyAMA0", nullptr), Status::Ok);
  CHECK(vm->runSync(4) == StopReason::Halted);

  const std::vector<uint8_t> corrupt = {0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00};
  Status invalidStatus;
  auto anotherVm = Vm::create(cfg, &invalidStatus);
  CHECK_ST(invalidStatus, Status::Ok);
  if (anotherVm)
    CHECK_ST(anotherVm->loadLinuxImage(corrupt.data(), corrupt.size()), Status::InvalidArgument);
}

NT_TEST(stage4d_linux_image_loader_rejects_bad_or_unsupported_images) {
  Bus bus;
  CHECK_ST(bus.addRam(GuestAddr(0x40000000ull), 16ull << 20), Status::Ok);
  std::vector<uint8_t> image(64, 0);
  Arm64LinuxImageInfo info;
  CHECK_ST(inspectArm64LinuxImage(image.data(), image.size(), 0x40000000ull, 16ull << 20, &info),
           Status::InvalidArgument); // missing Image magic
  putLe32(image, 56, 0x644D5241u);
  putLe64(image, 8, 0x80000ull);
  putLe64(image, 16, 0x1000ull);
  putLe64(image, 24, 0x4ull); // 16 KiB kernel page size not supported
  CHECK_ST(inspectArm64LinuxImage(image.data(), image.size(), 0x40000000ull, 16ull << 20, &info),
           Status::InvalidArgument);
}
