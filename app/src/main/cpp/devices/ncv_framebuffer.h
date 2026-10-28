#pragma once
#include <cstdint>
#include <atomic>
#include <mutex>
#include <vector>
#include "../memory/ncv_bus.h"

namespace ncv {

// Simple virtual framebuffer exposed to Linux as a simple-framebuffer device.
// This is a software pixel buffer only, not Mali/GPU or Android HWC emulation.
class GuestFramebuffer final : public Device {
 public:
  static constexpr uint64_t kBase = 0x3F000000ull;
  static constexpr uint32_t kWidth = 640;
  static constexpr uint32_t kHeight = 480;
  static constexpr uint32_t kBytesPerPixel = 4;
  static constexpr uint32_t kStride = kWidth * kBytesPerPixel;
  static constexpr uint64_t kRegionSize = static_cast<uint64_t>(kStride) * kHeight;
  const char* name() const override { return "ncv-simple-framebuffer"; }
  Status read(uint64_t offset, unsigned size, uint64_t* value) override;
  Status write(uint64_t offset, unsigned size, uint64_t value) override;
  std::vector<uint32_t> snapshotArgb() const;
  void clear();
  uint64_t readOperations() const { return readOps_.load(std::memory_order_relaxed); }
  uint64_t writeOperations() const { return writeOps_.load(std::memory_order_relaxed); }
  uint64_t nonZeroWriteOperations() const { return nonZeroWriteOps_.load(std::memory_order_relaxed); }
 private:
  mutable std::mutex mu_;
  std::vector<uint8_t> bytes_ = std::vector<uint8_t>(static_cast<size_t>(kRegionSize), 0);
  std::atomic<uint64_t> readOps_{0};
  std::atomic<uint64_t> writeOps_{0};
  std::atomic<uint64_t> nonZeroWriteOps_{0};
};

} // namespace ncv
