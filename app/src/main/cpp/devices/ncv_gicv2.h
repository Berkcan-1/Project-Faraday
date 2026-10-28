// NCV single-vCPU GICv2 model for the virtual board.
// Implements a 256-ID distributor register window and a basic CPU interface.
// SGI delivery is single-CPU only; security extensions, routing and SMP are not modeled.
#pragma once
#include <array>
#include <cstdint>
#include <mutex>
#include "../memory/ncv_bus.h"

namespace ncv {

class GicV2 final : public Device {
 public:
  GicV2();
  static constexpr uint64_t kRegionSize = 0x20000; // GICD at +0, GICC at +0x10000
  static constexpr unsigned kInterruptCount = 256;
  static constexpr unsigned kRegisterBanks = kInterruptCount / 32;
  static constexpr unsigned kConfigWords = kInterruptCount / 16;
  static constexpr unsigned kPhysicalTimerPpi = 30;

  const char* name() const override { return "gicv2-ncv-256"; }
  Status read(uint64_t offset, unsigned size, uint64_t* value) override;
  Status write(uint64_t offset, unsigned size, uint64_t value) override;

  void setLevel(unsigned irq, bool asserted);
  bool hasDeliverableInterrupt() const;
  uint32_t acknowledge();
  void endOfInterrupt(uint32_t irq);
  void reset();

 private:
  static bool validAccess(unsigned size, uint64_t offset);
  static bool byteAccessAllowed(uint64_t offset);
  uint32_t read32(uint64_t offset);
  Status write32(uint64_t offset, uint32_t value);
  uint32_t highestPending() const;
  bool targeted(unsigned irq) const;
  bool enabled(unsigned irq) const;
  bool pending(unsigned irq) const;
  bool active(unsigned irq) const;
  uint8_t priority(unsigned irq) const;
  void setPending(unsigned irq, bool value);
  void setEnabled(unsigned irq, bool value);
  uint32_t readWordBytes(const uint8_t* bytes, unsigned count, unsigned first) const;
  void writeWordBytes(uint8_t* bytes, unsigned count, unsigned first, uint32_t value);
  uint32_t readSgiPending(unsigned firstSgi) const;
  void writeSgiPending(unsigned firstSgi, uint32_t value, bool set);

  bool distEnabled_ = false;
  bool cpuEnabled_ = false;
  uint32_t distributorControlValue_ = 0;
  uint32_t cpuControlValue_ = 0;
  uint8_t priorityMask_ = 0xFF;
  uint8_t binaryPoint_ = 0;
  uint32_t activeIrq_ = 1023;
  mutable std::recursive_mutex mu_;
  std::array<uint32_t, kRegisterBanks> enable_{};
  std::array<uint32_t, kRegisterBanks> pending_{};
  std::array<uint32_t, kRegisterBanks> active_{};
  std::array<uint32_t, kRegisterBanks> line_{};
  std::array<uint32_t, kRegisterBanks> group_{};
  std::array<uint32_t, 4> activePriority_{};
  std::array<uint32_t, 4> nonSecureActivePriority_{};
  std::array<uint32_t, kConfigWords> config_{};
  std::array<uint8_t, kInterruptCount> priorities_{};
  std::array<uint8_t, kInterruptCount> targets_{};
};

} // namespace ncv
