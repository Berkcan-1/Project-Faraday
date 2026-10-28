#include "ncv_pl031_rtc.h"
#include <chrono>

namespace ncv {
namespace {
constexpr uint64_t DR=0x00, MR=0x04, LR=0x08, CR=0x0C, IMSC=0x10, RIS=0x14, MIS=0x18, ICR=0x1C;
int64_t epochSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
bool validSize(unsigned size) { return size == 4; }
}

uint32_t Pl031Rtc::counterLocked() const {
  if ((control_ & 1u) == 0) return loadValue_;
  const int64_t elapsed = epochSeconds() - loadEpochSeconds_;
  return loadValue_ + static_cast<uint32_t>(elapsed > 0 ? elapsed : 0);
}
void Pl031Rtc::updateMatchLocked(uint32_t now) {
  if (lastCounter_ < matchValue_ && now >= matchValue_) matchPending_ = true;
  lastCounter_ = now;
}
void Pl031Rtc::signalIrq() {
  std::function<void(bool)> sink; bool level = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    const uint32_t now = counterLocked(); updateMatchLocked(now);
    sink = irqSink_; level = matchPending_ && (interruptMask_ & 1u) != 0;
  }
  if (sink) sink(level);
}
Status Pl031Rtc::read(uint64_t offset, unsigned size, uint64_t* value) {
  if (!value || !validSize(size) || (offset & 3u)) return Status::DeviceError;
  bool irqUpdate = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    const uint32_t now = counterLocked(); updateMatchLocked(now);
    switch (offset) {
      case DR: *value = now; break;
      case MR: *value = matchValue_; break;
      case LR: *value = loadValue_; break;
      case CR: *value = control_; break;
      case IMSC: *value = interruptMask_; break;
      case RIS: *value = matchPending_ ? 1u : 0u; break;
      case MIS: *value = (matchPending_ && (interruptMask_ & 1u)) ? 1u : 0u; break;
      case ICR: *value = 0; break;
      case 0xFE0: *value = 0x31; break;
      case 0xFE4: *value = 0x10; break;
      case 0xFE8: *value = 0x14; break;
      case 0xFEC: *value = 0x00; break;
      case 0xFF0: *value = 0x0D; break;
      case 0xFF4: *value = 0xF0; break;
      case 0xFF8: *value = 0x05; break;
      case 0xFFC: *value = 0xB1; break;
      default: return Status::DeviceError;
    }
    irqUpdate = offset == DR || offset == MIS || offset == RIS;
  }
  if (irqUpdate) signalIrq();
  return Status::Ok;
}
Status Pl031Rtc::write(uint64_t offset, unsigned size, uint64_t value) {
  if (!validSize(size) || (offset & 3u)) return Status::DeviceError;
  {
    std::lock_guard<std::mutex> lock(mu_);
    const uint32_t now = counterLocked(); updateMatchLocked(now);
    switch (offset) {
      case MR: matchValue_ = static_cast<uint32_t>(value); matchPending_ = false; lastCounter_ = now; break;
      case LR: loadValue_ = static_cast<uint32_t>(value); loadEpochSeconds_ = epochSeconds(); lastCounter_ = loadValue_; matchPending_ = false; break;
      case CR: control_ = static_cast<uint32_t>(value) & 1u; if (control_) loadEpochSeconds_ = epochSeconds(); break;
      case IMSC: interruptMask_ = static_cast<uint32_t>(value) & 1u; break;
      case ICR: if (value & 1u) matchPending_ = false; break;
      case DR: case RIS: case MIS: return Status::DeviceError;
      default:
        if (offset >= 0xFE0 && offset <= 0xFFC) return Status::Ok;
        return Status::DeviceError;
    }
  }
  signalIrq();
  return Status::Ok;
}
void Pl031Rtc::poll() { signalIrq(); }
void Pl031Rtc::setIrqSink(std::function<void(bool)> sink) {
  { std::lock_guard<std::mutex> lock(mu_); irqSink_ = std::move(sink); }
  signalIrq();
}
void Pl031Rtc::reset() {
  { std::lock_guard<std::mutex> lock(mu_); loadValue_=0; matchValue_=0xFFFFFFFFu; loadEpochSeconds_=epochSeconds(); control_=1; interruptMask_=0; matchPending_=false; lastCounter_=0; }
  signalIrq();
}
} // namespace ncv
