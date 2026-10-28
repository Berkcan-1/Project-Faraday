#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <random>
#include "../memory/ncv_bus.h"

namespace ncv {

// Minimal modern virtio-mmio RNG (device ID 4), one split ring queue.
// Host entropy is copied only into guest RAM descriptors explicitly supplied by the guest.
class VirtioMmioRng final : public Device {
 public:
  static constexpr uint64_t kBase = 0x0A002000ull;
  static constexpr uint64_t kRegionSize = 0x200;
  static constexpr uint32_t kMaxQueueSize = 128;
  explicit VirtioMmioRng(Bus* bus) : bus_(bus) {}
  const char* name() const override { return "virtio-mmio-rng"; }
  Status read(uint64_t offset, unsigned size, uint64_t* value) override;
  Status write(uint64_t offset, unsigned size, uint64_t value) override;
  void setIrqSink(std::function<void(bool)> sink);
  void reset();
 private:
  Status processQueue();
  Status readGuest(uint64_t address, void* out, size_t size);
  Status writeGuest(uint64_t address, const void* in, size_t size);
  void updateIrq();
  Bus* bus_ = nullptr;
  mutable std::mutex mu_;
  std::function<void(bool)> irqSink_;
  uint32_t deviceFeaturesSel_=0, driverFeaturesSel_=0;
  uint32_t driverFeatures_[2]{};
  uint32_t status_=0, interruptStatus_=0;
  uint32_t queueNum_=0, queueReady_=0;
  uint64_t queueDesc_=0, queueAvail_=0, queueUsed_=0;
  uint16_t lastAvailIndex_=0, usedIndex_=0;
};

} // namespace ncv
