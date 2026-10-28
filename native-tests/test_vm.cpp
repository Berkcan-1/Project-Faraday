#include <chrono>
#include <memory>
#include <thread>

#include "core/ncv_vm.h"
#include "generated/test_programs.h"
#include "selftest/ncv_payload_hello.h"
#include "test_framework.h"

using namespace ncv;
using namespace ncvgen;

namespace {
const uint64_t kBase = BoardConfig().ramBase;

std::unique_ptr<Vm> makeVm(const uint8_t* payload, size_t len, uint64_t ram = 1 << 20) {
  BoardConfig cfg;
  cfg.ramSize = ram;
  Status st;
  auto vm = Vm::create(cfg, &st);
  CHECK_ST(st, Status::Ok);
  if (vm) CHECK_ST(vm->loadPayload(GuestAddr(kBase), payload, len), Status::Ok);
  return vm;
}

bool waitState(Vm& vm, VmState want, int timeoutMs = 5000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < end) {
    if (vm.state() == want) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
}  // namespace

NT_TEST(vm_create_validates_board) {
  Status st;
  BoardConfig bad; bad.ramSize = 0;
  CHECK(Vm::create(bad, &st) == nullptr); CHECK_ST(st, Status::InvalidArgument);
  BoardConfig overlap; overlap.uartBase = overlap.ramBase + 0x1000;      // UART RAM'in içine düşüyor
  CHECK(Vm::create(overlap, &st) == nullptr); CHECK_ST(st, Status::Overlap);
  BoardConfig twoGiB; twoGiB.ramSize = 2048ull << 20;                      // 2 GiB guest capacity is lazily backed
  auto largeVm = Vm::create(twoGiB, &st);
  CHECK(largeVm != nullptr); CHECK_ST(st, Status::Ok);
  if (largeVm) {
    CHECK_EQ(largeVm->bus().totalRamBytes(), 2048ull << 20);
    CHECK_EQ(largeVm->bus().residentRamBytes(), 0ull);
  }
}

NT_TEST(vm_sync_hello_reset_rerun) {
  auto vm = makeVm(kHelloPayload, kHelloPayload_len);
  CHECK(vm->state() == VmState::Created);
  CHECK(vm->runSync(100000) == StopReason::Halted);
  CHECK(vm->state() == VmState::Stopped);
  CHECK(vm->consoleOutput() == "NCV GUEST BOOT OK\n");
  CHECK(vm->lastStop().reason == StopReason::Halted);
  // Geçersiz geçişler
  CHECK_ST(vm->start(), Status::InvalidState);
  CHECK_ST(vm->pause(), Status::InvalidState);
  CHECK_ST(vm->loadPayload(GuestAddr(kBase), kHelloPayload, kHelloPayload_len), Status::InvalidState);
  // reset: RAM + konsol temizlenir, payload yeniden yüklenmeli
  CHECK_ST(vm->reset(), Status::Ok);
  CHECK(vm->state() == VmState::Created);
  CHECK(vm->consoleOutput().empty());
  uint64_t v = 1;
  CHECK_ST(vm->bus().read(GuestAddr(kBase), 4, &v), Status::Ok);
  CHECK_EQ(v, 0ull);
  CHECK_ST(vm->loadPayload(GuestAddr(kBase), kHelloPayload, kHelloPayload_len), Status::Ok);
  CHECK(vm->runSync(100000) == StopReason::Halted);
  CHECK(vm->consoleOutput() == "NCV GUEST BOOT OK\n");
}

NT_TEST(vm_invalid_transitions_from_created) {
  auto vm = makeVm(kHelloPayload, kHelloPayload_len);
  CHECK_ST(vm->pause(), Status::InvalidState);
  CHECK_ST(vm->resume(), Status::InvalidState);
  CHECK_ST(vm->stop(), Status::InvalidState);
  CHECK_ST(vm->reset(), Status::InvalidState);
  CHECK_ST(vm->loadPayload(GuestAddr(kBase), nullptr, 4), Status::InvalidArgument);
  CHECK_ST(vm->loadPayload(GuestAddr(0x1000), kHelloPayload, kHelloPayload_len), Status::Unmapped);
}

NT_TEST(vm_sync_slices_accumulate) {
  auto vm = makeVm(prog_loop_forever, prog_loop_forever_len);
  CHECK(vm->runSync(10) == StopReason::InstructionLimit);
  CHECK(vm->state() == VmState::Paused);
  CHECK(vm->runSync(10) == StopReason::InstructionLimit);
  CHECK_EQ(vm->instructionsRetired(), 20ull);
}

NT_TEST(vm_sync_fault_state) {
  auto vm = makeVm(prog_fault_load, prog_fault_load_len);
  CHECK(vm->runSync(100) == StopReason::BusFault);
  CHECK(vm->state() == VmState::Faulted);
  CHECK_ST(vm->lastStop().busStatus, Status::Unmapped);
  CHECK_ST(vm->reset(), Status::Ok);
}

NT_TEST(vm_async_hello) {
  auto vm = makeVm(kHelloPayload, kHelloPayload_len);
  CHECK_ST(vm->start(), Status::Ok);
  CHECK_ST(vm->start(), Status::InvalidState);              // ikinci start reddedilir
  CHECK(waitState(*vm, VmState::Stopped));
  CHECK(vm->consoleOutput() == "NCV GUEST BOOT OK\n");
  CHECK(vm->lastStop().reason == StopReason::Halted);
  CHECK_ST(vm->stop(), Status::Ok);                         // idempotent, thread toplanır
  CHECK_ST(vm->stop(), Status::Ok);
  CHECK_ST(vm->reset(), Status::Ok);
}

NT_TEST(vm_async_fault) {
  auto vm = makeVm(prog_fault_store, prog_fault_store_len);
  CHECK_ST(vm->start(), Status::Ok);
  CHECK(waitState(*vm, VmState::Faulted));
  CHECK(vm->lastStop().reason == StopReason::BusFault);
  CHECK_ST(vm->lastStop().busStatus, Status::Unmapped);
  CHECK_ST(vm->stop(), Status::Ok);
}

NT_TEST(vm_pause_resume_stop_reset_cycle) {
  auto vm = makeVm(prog_loop_forever, prog_loop_forever_len);
  CHECK_ST(vm->start(), Status::Ok);
  sleepMs(30);
  CHECK(vm->state() == VmState::Running);
  CpuState cs;
  CHECK_ST(vm->cpuState(&cs), Status::InvalidState);        // Running iken okunamaz
  CHECK_ST(vm->pause(), Status::Ok);
  CHECK(vm->state() == VmState::Paused);
  const uint64_t r1 = vm->instructionsRetired();
  CHECK(r1 > 0);
  sleepMs(30);
  CHECK_EQ(vm->instructionsRetired(), r1);                  // pause döndükten sonra CPU ilerlemez
  CHECK_ST(vm->cpuState(&cs), Status::Ok);
  CHECK_EQ(cs.pc, kBase);
  CHECK_ST(vm->pause(), Status::InvalidState);              // zaten Paused
  CHECK_ST(vm->resume(), Status::Ok);
  sleepMs(30);
  CHECK_ST(vm->pause(), Status::Ok);
  CHECK(vm->instructionsRetired() > r1);                    // resume sonrası ilerledi
  CHECK_ST(vm->stop(), Status::Ok);
  CHECK(vm->state() == VmState::Stopped);
  CHECK(vm->lastStop().reason == StopReason::StoppedByHost);
  CHECK_ST(vm->start(), Status::InvalidState);              // Stopped'dan doğrudan start yok
  CHECK_ST(vm->reset(), Status::Ok);
  CHECK(vm->state() == VmState::Created);
  CHECK_ST(vm->loadPayload(GuestAddr(kBase), kHelloPayload, kHelloPayload_len), Status::Ok);
  CHECK_ST(vm->start(), Status::Ok);                        // reset sonrası yeniden başlatılabilir
  CHECK(waitState(*vm, VmState::Stopped));
  CHECK(vm->consoleOutput() == "NCV GUEST BOOT OK\n");
}

NT_TEST(vm_stop_while_paused) {
  auto vm = makeVm(prog_loop_forever, prog_loop_forever_len);
  CHECK_ST(vm->start(), Status::Ok);
  sleepMs(10);
  CHECK_ST(vm->pause(), Status::Ok);
  CHECK_ST(vm->stop(), Status::Ok);
  CHECK(vm->state() == VmState::Stopped);
}

// Çalışırken yok edilirse destructor takılmamalı (test koşucusu timeout ile çalıştırılır).
NT_TEST(vm_destroy_while_running_and_paused) {
  for (int i = 0; i < 20; i++) {
    auto vm = makeVm(prog_loop_forever, prog_loop_forever_len);
    CHECK_ST(vm->start(), Status::Ok);
    if (i % 2) { sleepMs(2); CHECK_ST(vm->pause(), Status::Ok); }
    vm.reset();
  }
}

NT_TEST(vm_repeated_pause_resume_stress) {
  auto vm = makeVm(prog_loop_forever, prog_loop_forever_len);
  CHECK_ST(vm->start(), Status::Ok);
  for (int i = 0; i < 200; i++) {
    CHECK_ST(vm->pause(), Status::Ok);
    CHECK_ST(vm->resume(), Status::Ok);
  }
  CHECK_ST(vm->stop(), Status::Ok);
}

NT_TEST(vm_live_runtime_snapshot_reports_guest_progress) {
  auto vm = makeVm(prog_loop_forever, prog_loop_forever_len);
  CHECK_ST(vm->start(), Status::Ok);
  sleepMs(10);
  CHECK_ST(vm->pause(), Status::Ok);
  const std::string snapshot = vm->runtimeDiagnostics();
  CHECK(snapshot.find("state=Paused") != std::string::npos);
  CHECK(snapshot.find("pc=0x") != std::string::npos);
  CHECK(snapshot.find("last_insn=0x") != std::string::npos);
  CHECK(snapshot.find("retired=") != std::string::npos);
  CHECK(snapshot.find("slices=") != std::string::npos);
  CHECK(vm->instructionsRetired() > 0);
  CHECK_ST(vm->stop(), Status::Ok);
}

NT_TEST(vm_console_input_reaches_uart) {
  auto vm = makeVm(kHelloPayload, kHelloPayload_len);
  CHECK(vm->pushConsoleInput('q'));
  Bus& b = vm->bus();
  uint64_t v = 0;
  CHECK_ST(b.read(GuestAddr(BoardConfig().uartBase), 4, &v), Status::Ok);
  CHECK_EQ(v, (uint64_t)'q');
}
