#include "ncv_uart.h"

#include <utility>

namespace ncv {
namespace {
constexpr uint64_t kDR = 0x000, kFR = 0x018, kIBRD = 0x024, kFBRD = 0x028, kLCRH = 0x02C,
                   kCR = 0x030, kIFLS = 0x034, kIMSC = 0x038, kRIS = 0x03C,
                   kMIS = 0x040, kICR = 0x044;
constexpr uint32_t kFR_RXFE = 1u << 4, kFR_RXFF = 1u << 6, kFR_TXFE = 1u << 7;
constexpr uint32_t kIntRx = 1u << 4, kIntTx = 1u << 5;
constexpr uint64_t kPeriphId0 = 0xFE0, kPeriphId1 = 0xFE4, kPeriphId2 = 0xFE8, kPeriphId3 = 0xFEC;
constexpr uint64_t kCellId0 = 0xFF0, kCellId1 = 0xFF4, kCellId2 = 0xFF8, kCellId3 = 0xFFC;
}  // namespace

bool Pl011Uart::irqAssertedLocked() const {
  uint32_t raw = kIntTx;  // TX FIFO is drained immediately in this model.
  if (!rx_.empty()) raw |= kIntRx;
  return (raw & imsc_) != 0;
}

void Pl011Uart::updateIrq() {
  std::function<void(bool)> sink;
  bool asserted = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    asserted = irqAssertedLocked();
    sink = irqSink_;
  }
  if (sink) sink(asserted);
}

Status Pl011Uart::read(uint64_t offset, unsigned size, uint64_t* value) {
  if (value == nullptr || size == 0 || size > 4 || (offset & 3) != 0) return Status::DeviceError;
  bool irqChanged = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    switch (offset) {
      case kDR:
        if (rx_.empty()) *value = 0;
        else { *value = rx_.front(); rx_.pop_front(); }
        irqChanged = true;
        break;
      case kFR: {
        uint32_t v = kFR_TXFE;  // TX anında tüketilir -> hep boş
        if (rx_.empty()) v |= kFR_RXFE;
        if (rx_.size() >= kRxFifoDepth) v |= kFR_RXFF;
        *value = v;
        break;
      }
      case kIBRD: *value = ibrd_; break;
      case kFBRD: *value = fbrd_; break;
      case kLCRH: *value = lcrh_; break;
      case kCR: *value = cr_; break;
      case kIFLS: *value = 0; break; // fixed default FIFO trigger profile
      case kIMSC: *value = imsc_; break;
      case kRIS: *value = (kIntTx | (rx_.empty() ? 0u : kIntRx)); break;
      case kMIS: *value = (kIntTx | (rx_.empty() ? 0u : kIntRx)) & imsc_; break;
      case kPeriphId0: *value = 0x11; break;
      case kPeriphId1: *value = 0x10; break;
      case kPeriphId2: *value = 0x14; break;
      case kPeriphId3: *value = 0x00; break;
      case kCellId0: *value = 0x0D; break;
      case kCellId1: *value = 0xF0; break;
      case kCellId2: *value = 0x05; break;
      case kCellId3: *value = 0xB1; break;
      default: return Status::DeviceError; // uygulanmamış register
    }
  }
  if (irqChanged) updateIrq();
  return Status::Ok;
}

Status Pl011Uart::write(uint64_t offset, unsigned size, uint64_t value) {
  if (size == 0 || size > 4 || (offset & 3) != 0) return Status::DeviceError;
  if (offset == kDR) {
    const uint8_t b = static_cast<uint8_t>(value & 0xFF);
    std::function<void(uint8_t)> sink;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (tx_.size() < maxTx_) tx_.push_back(static_cast<char>(b));
      else dropped_++;
      sink = sink_;
    }
    if (sink) sink(b);  // kilit dışında çağrılır
    updateIrq();
    return Status::Ok;
  }

  bool irqChanged = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    switch (offset) {
      case kIBRD: ibrd_ = static_cast<uint32_t>(value); break;
      case kFBRD: fbrd_ = static_cast<uint32_t>(value); break;
      case kLCRH: lcrh_ = static_cast<uint32_t>(value); break;
      case kCR: cr_ = static_cast<uint32_t>(value); break;
      case kIFLS: break; // simplified model uses a fixed RX trigger
      case kIMSC: imsc_ = static_cast<uint32_t>(value) & 0x7FFu; irqChanged = true; break;
      case kICR: irqChanged = true; break; // level causes remain asserted while their condition holds
      case kPeriphId0: case kPeriphId1: case kPeriphId2: case kPeriphId3:
      case kCellId0: case kCellId1: case kCellId2: case kCellId3:
        break; // read-only ID registers, writes are ignored
      default: return Status::DeviceError;
    }
  }
  if (irqChanged) updateIrq();
  return Status::Ok;
}

bool Pl011Uart::pushRx(uint8_t byte) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (rx_.size() >= kRxFifoDepth) return false;
    rx_.push_back(byte);
  }
  updateIrq();
  return true;
}

std::string Pl011Uart::peekOutput() const {
  std::lock_guard<std::mutex> lk(mu_);
  return tx_;
}

std::string Pl011Uart::takeOutput() {
  std::lock_guard<std::mutex> lk(mu_);
  std::string out;
  out.swap(tx_);
  return out;
}

uint64_t Pl011Uart::droppedTxBytes() const {
  std::lock_guard<std::mutex> lk(mu_);
  return dropped_;
}

void Pl011Uart::setTxSink(std::function<void(uint8_t)> sink) {
  std::lock_guard<std::mutex> lk(mu_);
  sink_ = std::move(sink);
}

void Pl011Uart::setIrqSink(std::function<void(bool)> sink) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    irqSink_ = std::move(sink);
  }
  updateIrq();
}

void Pl011Uart::clear() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    tx_.clear();
    rx_.clear();
    dropped_ = 0;
    ibrd_ = fbrd_ = lcrh_ = cr_ = imsc_ = 0;
  }
  updateIrq();
}

}  // namespace ncv
