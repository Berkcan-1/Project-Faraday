#include "ncv_selftest.h"

#include "../core/ncv_version.h"
#include "../core/ncv_vm.h"
#include "ncv_payload_hello.h"

namespace ncv {

std::string runSelfTest(bool* ok) {
  bool allOk = true;
  std::string rep = std::string("engine=") + NCV_ENGINE_VERSION + "\n";
  auto check = [&](const char* name, bool pass, const std::string& detail) {
    rep += std::string(pass ? "PASS " : "FAIL ") + name;
    if (!detail.empty()) rep += " (" + detail + ")";
    rep += "\n";
    if (!pass) allOk = false;
  };

  // 1) Hello payload: çıktı emüle edilen guest kodundan gelmeli.
  {
    BoardConfig cfg;
    cfg.ramSize = 1ull << 20;
    Status st;
    std::unique_ptr<Vm> vm = Vm::create(cfg, &st);
    bool pass = false;
    std::string detail;
    if (!vm) {
      detail = std::string("create: ") + statusName(st);
    } else if ((st = vm->loadPayload(GuestAddr(cfg.ramBase), kHelloPayload, kHelloPayload_len)) != Status::Ok) {
      detail = std::string("load: ") + statusName(st);
    } else {
      const StopReason r = vm->runSync(100000);
      const std::string out = vm->consoleOutput();
      pass = (r == StopReason::Halted && out == "NCV GUEST BOOT OK\n");
      detail = std::string("stop=") + stopReasonName(r);
    }
    check("guest-hello-uart", pass, detail);
  }

  // 2) Geçersiz erişim host'u çökertmeden kontrollü hata vermeli.
  {
    BoardConfig cfg;
    cfg.ramSize = 4096;
    Status st;
    std::unique_ptr<Vm> vm = Vm::create(cfg, &st);
    bool pass = false;
    if (vm) {
      uint64_t v = 0;
      pass = vm->bus().read(GuestAddr(0x1000), 8, &v) == Status::Unmapped &&
             vm->bus().read(GuestAddr(cfg.ramBase + 4092), 8, &v) == Status::OutOfBounds;
    }
    check("bus-bounds", pass, "");
  }

  if (ok) *ok = allOk;
  return rep;
}

}  // namespace ncv
