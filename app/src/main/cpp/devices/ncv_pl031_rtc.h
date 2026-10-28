#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include "../memory/ncv_bus.h"

namespace ncv {

// ARM PL031-compatible RTC for the NCV virtual board. It uses host wall time as a
// guest RTC source; it is not an emulator of any MediaTek physical RTC.
class Pl031Rtc final : public Device {
 public:
  static constexpr uint64_t kRegionSize = 0x1000;
  const char* name() const override { return "pl031-rtc"; }
  Status read(uint64_t offset, unsigned size, uint64_t* value) override;
  Status write(uint64_t offset, unsigned size, uint64_t value) override;
  void setIrqSink(std::function<void(bool)> sink);
  void poll();
  void reset();
 private:
  uint32_t counterLocked() const;
  void updateMatchLocked(uint32_t now);
  void signalIrq();
  mutable std::mutex mu_;
  uint32_t loadValue_ = 0, matchValue_ = 0xFFFFFFFFu;
  int64_t loadEpochSeconds_ = 0;
  uint32_t control_ = 1, interruptMask_ = 0;
  bool matchPending_ = false;
  uint32_t lastCounter_ = 0;
  std::function<void(bool)> irqSink_;
};

} // namespace ncv
