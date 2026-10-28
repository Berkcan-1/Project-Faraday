// Diagnostic MediaTek MMIO fallback. This is NOT a full MTK SoC or driver emulator.
#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include "../memory/ncv_bus.h"

namespace ncv {

class MtkProbeMmioShim final : public Device {
 public:
  const char* name() const override { return "mtk-probe-mmio-shim"; }
  Status read(uint64_t physicalAddress, unsigned size, uint64_t* value) override;
  Status write(uint64_t physicalAddress, unsigned size, uint64_t value) override;
  std::string report() const;
  void resetCounters();
 private:
  struct Trace { char op='?'; uint64_t address=0, value=0; unsigned size=0; };
  void record(char op, uint64_t address, unsigned size, uint64_t value);
  mutable std::mutex mu_;
  uint64_t reads_=0, writes_=0, rejectedReads_=0, rejectedWrites_=0;
  uint64_t lastRead_=0, lastWrite_=0, lastRejectedRead_=0, lastRejectedWrite_=0;
  std::deque<Trace> recent_;
};

} // namespace ncv
