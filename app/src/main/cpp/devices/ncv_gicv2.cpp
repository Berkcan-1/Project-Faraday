#include "ncv_gicv2.h"
#include <algorithm>

namespace ncv {
namespace {
constexpr uint32_t kNoInterrupt = 1023u;
constexpr uint32_t kIidr = 0x0202143Bu;
// ITLinesNumber = 7 means 8 banks * 32 interrupt IDs = 256 implemented IDs.
constexpr uint32_t kGicDtyper = 7u;
}

GicV2::GicV2() { reset(); }

void GicV2::reset() {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  distEnabled_ = false;
  cpuEnabled_ = false;
  distributorControlValue_ = 0;
  cpuControlValue_ = 0;
  priorityMask_ = 0xFF;
  binaryPoint_ = 0;
  activeIrq_ = kNoInterrupt;
  enable_.fill(0);
  pending_.fill(0);
  active_.fill(0);
  line_.fill(0);
  group_.fill(0xFFFFFFFFu); // simplified non-secure Group 1 model
  activePriority_.fill(0);
  nonSecureActivePriority_.fill(0);
  config_.fill(0);
  priorities_.fill(0x80);
  targets_.fill(0);
  for (unsigned irq = 32; irq < kInterruptCount; ++irq) targets_[irq] = 1;
}

bool GicV2::byteAccessAllowed(uint64_t offset) {
  // GICv2 explicitly supports byte accesses for IPRIORITYR, ITARGETSR,
  // CPENDSGIR and SPENDSGIR. Other registers remain 32-bit word-only.
  return (offset >= 0x400 && offset < 0x500) ||
         (offset >= 0x800 && offset < 0x900) ||
         (offset >= 0xF10 && offset < 0xF30);
}

bool GicV2::validAccess(unsigned size, uint64_t offset) {
  if (offset >= kRegionSize) return false;
  if (size == 4) return (offset & 3u) == 0;
  return size == 1 && byteAccessAllowed(offset);
}

bool GicV2::enabled(unsigned irq) const {
  return irq < kInterruptCount && ((enable_[irq / 32] >> (irq % 32)) & 1u) != 0;
}
bool GicV2::pending(unsigned irq) const {
  return irq < kInterruptCount && ((pending_[irq / 32] >> (irq % 32)) & 1u) != 0;
}
bool GicV2::active(unsigned irq) const {
  return irq < kInterruptCount && ((active_[irq / 32] >> (irq % 32)) & 1u) != 0;
}
uint8_t GicV2::priority(unsigned irq) const {
  return irq < kInterruptCount ? priorities_[irq] : 0xFF;
}
void GicV2::setPending(unsigned irq, bool value) {
  if (irq >= kInterruptCount) return;
  const uint32_t bit = 1u << (irq % 32);
  if (value) pending_[irq / 32] |= bit;
  else pending_[irq / 32] &= ~bit;
}
void GicV2::setEnabled(unsigned irq, bool value) {
  if (irq >= kInterruptCount) return;
  const uint32_t bit = 1u << (irq % 32);
  if (value) enable_[irq / 32] |= bit;
  else enable_[irq / 32] &= ~bit;
}
bool GicV2::targeted(unsigned irq) const {
  if (irq < 16) return true; // SGIs are routed only to the single virtual CPU
  if (irq < 32) return true; // PPIs are banked to the single virtual CPU
  return (targets_[irq] & 1u) != 0;
}

void GicV2::setLevel(unsigned irq, bool asserted) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (irq >= kInterruptCount) return;
  const uint32_t bit = 1u << (irq % 32);
  if (asserted) {
    line_[irq / 32] |= bit;
    if (!active(irq)) setPending(irq, true);
  } else {
    line_[irq / 32] &= ~bit;
    if (!active(irq)) setPending(irq, false);
  }
}

uint32_t GicV2::highestPending() const {
  if (!distEnabled_ || !cpuEnabled_) return kNoInterrupt;
  unsigned chosen = kInterruptCount;
  uint8_t best = priorityMask_;
  for (unsigned irq = 0; irq < kInterruptCount; ++irq) {
    if (!targeted(irq)) continue;
    if (enabled(irq) && pending(irq) && !active(irq) && priority(irq) < best) {
      chosen = irq;
      best = priority(irq);
    }
  }
  return chosen < kInterruptCount ? chosen : kNoInterrupt;
}

bool GicV2::hasDeliverableInterrupt() const {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  return highestPending() != kNoInterrupt;
}

uint32_t GicV2::acknowledge() {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  const uint32_t chosen = highestPending();
  if (chosen >= kInterruptCount) return kNoInterrupt;
  setPending(chosen, false);
  active_[chosen / 32] |= 1u << (chosen % 32);
  activeIrq_ = chosen;
  return chosen;
}

void GicV2::endOfInterrupt(uint32_t irq) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  irq &= 0x3FFu;
  if (irq >= kInterruptCount) return;
  active_[irq / 32] &= ~(1u << (irq % 32));
  if (activeIrq_ == irq) activeIrq_ = kNoInterrupt;
  if ((line_[irq / 32] >> (irq % 32)) & 1u) setPending(irq, true);
}

uint32_t GicV2::readWordBytes(const uint8_t* bytes, unsigned count, unsigned first) const {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4 && first + i < count; ++i)
    value |= static_cast<uint32_t>(bytes[first + i]) << (i * 8);
  return value;
}
void GicV2::writeWordBytes(uint8_t* bytes, unsigned count, unsigned first, uint32_t value) {
  for (unsigned i = 0; i < 4 && first + i < count; ++i)
    bytes[first + i] = static_cast<uint8_t>(value >> (i * 8));
}
uint32_t GicV2::readSgiPending(unsigned firstSgi) const {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    const unsigned irq = firstSgi + i;
    // Single virtual CPU: source CPU ID is 0, in the least significant bit of each byte.
    if (irq < 16 && pending(irq)) value |= 1u << (i * 8);
  }
  return value;
}
void GicV2::writeSgiPending(unsigned firstSgi, uint32_t value, bool set) {
  for (unsigned i = 0; i < 4; ++i) {
    const unsigned irq = firstSgi + i;
    if (irq >= 16) break;
    if ((value & (1u << (i * 8))) != 0) setPending(irq, set);
  }
}

uint32_t GicV2::read32(uint64_t offset) {
  if (offset >= 0x10000) {
    const uint64_t r = offset - 0x10000;
    switch (r) {
      case 0x000: return cpuControlValue_;                       // GICC_CTLR
      case 0x004: return priorityMask_;                          // GICC_PMR
      case 0x008: return binaryPoint_;                           // GICC_BPR
      case 0x00C: return acknowledge();                          // GICC_IAR
      case 0x014: return activeIrq_ == kNoInterrupt ? 0xFFu : priority(activeIrq_); // GICC_RPR
      case 0x018: return highestPending();                       // GICC_HPPIR
      case 0x01C: return binaryPoint_;                           // GICC_ABPR approximation
      case 0x020: return acknowledge();                          // GICC_AIAR alias
      case 0x024: return 0;                                      // GICC_AEOIR write-only
      case 0x028: return highestPending();                       // GICC_AHPPIR
      case 0x02C: return 0;                                      // GICC_STATUSR, no errors modeled
      case 0x0FC: return kIidr;                                  // GICC_IIDR
      case 0x1000: return 0;                                     // GICC_DIR write-only
      default:
        if (r >= 0x0D0 && r <= 0x0DC && (r & 3u) == 0)
          return activePriority_[(r - 0x0D0) / 4];
        if (r >= 0x0E0 && r <= 0x0EC && (r & 3u) == 0)
          return nonSecureActivePriority_[(r - 0x0E0) / 4];
        return 0; // RAZ for unimplemented/reserved CPU-interface registers
    }
  }

  switch (offset) {
    case 0x000: return distributorControlValue_; // GICD_CTLR
    case 0x004: return kGicDtyper;              // GICD_TYPER: 256 interrupt IDs
    case 0x008: return kIidr;                    // GICD_IIDR
    case 0xF00: return 0;                        // GICD_SGIR is write-only
    default: break;
  }

  if (offset >= 0x080 && offset < 0x080 + kRegisterBanks * 4)
    return group_[(offset - 0x080) / 4];
  if (offset >= 0x100 && offset < 0x100 + kRegisterBanks * 4)
    return enable_[(offset - 0x100) / 4];
  if (offset >= 0x200 && offset < 0x200 + kRegisterBanks * 4)
    return pending_[(offset - 0x200) / 4];
  if (offset >= 0x300 && offset < 0x300 + kRegisterBanks * 4)
    return active_[(offset - 0x300) / 4];
  if (offset >= 0x400 && offset < 0x400 + kInterruptCount)
    return readWordBytes(priorities_.data(), priorities_.size(), static_cast<unsigned>(offset - 0x400));
  if (offset >= 0x800 && offset < 0x800 + kInterruptCount)
    return readWordBytes(targets_.data(), targets_.size(), static_cast<unsigned>(offset - 0x800));
  if (offset >= 0xC00 && offset < 0xC00 + kConfigWords * 4)
    return config_[(offset - 0xC00) / 4];
  if (offset >= 0xF10 && offset < 0xF20)
    return readSgiPending(static_cast<unsigned>((offset - 0xF10) / 4) * 4); // CPENDSGIR
  if (offset >= 0xF20 && offset < 0xF30)
    return readSgiPending(static_cast<unsigned>((offset - 0xF20) / 4) * 4); // SPENDSGIR

  // Read-as-zero for architecturally reserved/unimplemented offsets in the mapped GIC window.
  return 0;
}

Status GicV2::write32(uint64_t offset, uint32_t value) {
  if (offset >= 0x10000) {
    const uint64_t r = offset - 0x10000;
    switch (r) {
      case 0x000: cpuControlValue_ = value & 3u; cpuEnabled_ = cpuControlValue_ != 0; return Status::Ok;
      case 0x004: priorityMask_ = static_cast<uint8_t>(value); return Status::Ok;
      case 0x008: binaryPoint_ = static_cast<uint8_t>(value & 7u); return Status::Ok;
      case 0x010: endOfInterrupt(value); return Status::Ok;             // GICC_EOIR
      case 0x01C: binaryPoint_ = static_cast<uint8_t>(value & 7u); return Status::Ok;
      case 0x020: return Status::Ok;                                   // GICC_AIAR read-only
      case 0x024: endOfInterrupt(value); return Status::Ok;             // GICC_AEOIR
      case 0x028: return Status::Ok;                                   // GICC_AHPPIR read-only
      case 0x02C: return Status::Ok;                                   // GICC_STATUSR not modeled
      case 0x1000: endOfInterrupt(value); return Status::Ok;            // GICC_DIR
      default:
        if (r >= 0x0D0 && r <= 0x0DC && (r & 3u) == 0) {
          activePriority_[(r - 0x0D0) / 4] = value;
          return Status::Ok;
        }
        if (r >= 0x0E0 && r <= 0x0EC && (r & 3u) == 0) {
          nonSecureActivePriority_[(r - 0x0E0) / 4] = value;
          return Status::Ok;
        }
        return Status::Ok; // Write-ignored for unimplemented/reserved CPU-interface registers
    }
  }

  switch (offset) {
    case 0x000: distributorControlValue_ = value & 3u; distEnabled_ = distributorControlValue_ != 0; return Status::Ok;
    case 0xF00: { // GICD_SGIR; single-CPU target model
      const unsigned id = value & 0xFu;
      const unsigned filter = (value >> 24) & 3u;
      const bool cpu0Targeted = filter == 2u || (filter == 0u && ((value >> 16) & 1u) != 0);
      if (id < 16 && cpu0Targeted) setPending(id, true);
      return Status::Ok;
    }
    default: break;
  }

  if (offset >= 0x080 && offset < 0x080 + kRegisterBanks * 4) {
    group_[(offset - 0x080) / 4] = value;
    return Status::Ok;
  }
  if (offset >= 0x100 && offset < 0x100 + kRegisterBanks * 4) {
    enable_[(offset - 0x100) / 4] |= value;
    return Status::Ok;
  }
  if (offset >= 0x180 && offset < 0x180 + kRegisterBanks * 4) {
    enable_[(offset - 0x180) / 4] &= ~value;
    return Status::Ok;
  }
  if (offset >= 0x200 && offset < 0x200 + kRegisterBanks * 4) {
    pending_[(offset - 0x200) / 4] |= value;
    return Status::Ok;
  }
  if (offset >= 0x280 && offset < 0x280 + kRegisterBanks * 4) {
    pending_[(offset - 0x280) / 4] &= ~value;
    return Status::Ok;
  }
  if (offset >= 0x300 && offset < 0x300 + kRegisterBanks * 4) {
    active_[(offset - 0x300) / 4] |= value;
    return Status::Ok;
  }
  if (offset >= 0x380 && offset < 0x380 + kRegisterBanks * 4) {
    const unsigned bank = static_cast<unsigned>((offset - 0x380) / 4);
    active_[bank] &= ~value;
    for (unsigned bit = 0; bit < 32; ++bit) {
      const unsigned irq = bank * 32 + bit;
      if ((value & (1u << bit)) != 0) {
        if (activeIrq_ == irq) activeIrq_ = kNoInterrupt;
        if ((line_[bank] & (1u << bit)) != 0) setPending(irq, true);
      }
    }
    return Status::Ok;
  }
  if (offset >= 0x400 && offset < 0x400 + kInterruptCount) {
    writeWordBytes(priorities_.data(), priorities_.size(), static_cast<unsigned>(offset - 0x400), value);
    return Status::Ok;
  }
  if (offset >= 0x800 && offset < 0x800 + kInterruptCount) {
    const unsigned first = static_cast<unsigned>(offset - 0x800);
    for (unsigned i = 0; i < 4 && first + i < targets_.size(); ++i) {
      const unsigned irq = first + i;
      if (irq >= 32) targets_[irq] = (static_cast<uint8_t>(value >> (i * 8)) & 1u) ? 1u : 0u;
    }
    return Status::Ok;
  }
  if (offset >= 0xC00 && offset < 0xC00 + kConfigWords * 4) {
    config_[(offset - 0xC00) / 4] = value;
    return Status::Ok;
  }
  if (offset >= 0xF10 && offset < 0xF20) {
    writeSgiPending(static_cast<unsigned>((offset - 0xF10) / 4) * 4, value, false);
    return Status::Ok;
  }
  if (offset >= 0xF20 && offset < 0xF30) {
    writeSgiPending(static_cast<unsigned>((offset - 0xF20) / 4) * 4, value, true);
    return Status::Ok;
  }

  // Writes to reserved/unimplemented registers in a decoded GIC window are ignored;
  // bus accesses outside the mapped GIC region still fault in strict mode.
  return Status::Ok;
}

Status GicV2::read(uint64_t offset, unsigned size, uint64_t* value) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (value == nullptr || !validAccess(size, offset)) return Status::InvalidArgument;
  if (size == 1) {
    if (offset >= 0x400 && offset < 0x500) {
      *value = priorities_[static_cast<size_t>(offset - 0x400)];
    } else if (offset >= 0x800 && offset < 0x900) {
      *value = targets_[static_cast<size_t>(offset - 0x800)];
    } else {
      // CPENDSGIR/SPENDSGIR pack one source-CPU mask into each byte lane.
      const uint32_t word = read32(offset & ~uint64_t{3});
      *value = (word >> ((offset & 3u) * 8u)) & 0xFFu;
    }
    return Status::Ok;
  }
  *value = read32(offset);
  return Status::Ok;
}
Status GicV2::write(uint64_t offset, unsigned size, uint64_t value) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (!validAccess(size, offset)) return Status::InvalidArgument;
  if (size == 1) {
    const uint8_t byte = static_cast<uint8_t>(value & 0xFFu);
    if (offset >= 0x400 && offset < 0x500) {
      priorities_[static_cast<size_t>(offset - 0x400)] = byte;
      return Status::Ok;
    }
    if (offset >= 0x800 && offset < 0x900) {
      const unsigned irq = static_cast<unsigned>(offset - 0x800);
      // The first 32 IDs are SGI/PPI entries; preserve their modeled target.
      // For SPIs, this board only has CPU 0, represented by target bit 0.
      if (irq >= 32) targets_[irq] = (byte & 1u) ? 1u : 0u;
      return Status::Ok;
    }
    const uint64_t alignedOffset = offset & ~uint64_t{3};
    const unsigned shift = static_cast<unsigned>((offset & 3u) * 8u);
    return write32(alignedOffset, static_cast<uint32_t>(byte) << shift);
  }
  return write32(offset, static_cast<uint32_t>(value));
}

} // namespace ncv
