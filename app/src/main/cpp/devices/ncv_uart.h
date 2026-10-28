// NCV - minimal PL011-benzeri UART (ilk konsol aygıtı).
// KAPSAM: minimal DR/FR/config, RX/TX level IRQs and PrimeCell ID registers.
// Uygulanmamış register erişimi sessizce 0 dönmez; Status::DeviceError döner.
// Register offset'leri ARM PrimeCell PL011 TRM'ye göre yazıldı; TRM ile birebir doğrulama YAPILMADI.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

#include "../memory/ncv_bus.h"

namespace ncv {

class Pl011Uart : public Device {
 public:
  static constexpr uint64_t kRegionSize = 0x1000;
  static constexpr size_t kRxFifoDepth = 16;

  explicit Pl011Uart(size_t maxBufferedTxBytes = 64 * 1024) : maxTx_(maxBufferedTxBytes) {}

  const char* name() const override { return "pl011-subset"; }
  Status read(uint64_t offset, unsigned size, uint64_t* value) override;
  Status write(uint64_t offset, unsigned size, uint64_t value) override;

  // Host -> guest giriş. FIFO doluysa false.
  bool pushRx(uint8_t byte);
  // Guest'in yazdığı baytlar (thread-safe). peek: kopya, take: kopya + temizle.
  std::string peekOutput() const;
  std::string takeOutput();
  uint64_t droppedTxBytes() const;
  void setTxSink(std::function<void(uint8_t)> sink);  // opsiyonel; Vm çalışmıyorken ayarla
  void setIrqSink(std::function<void(bool)> sink);     // IRQ level callback, kilit dışında çağrılır
  void clear();

 private:
  bool irqAssertedLocked() const;
  void updateIrq();
  mutable std::mutex mu_;
  std::string tx_;
  uint64_t dropped_ = 0;
  size_t maxTx_;
  std::deque<uint8_t> rx_;
  uint32_t ibrd_ = 0, fbrd_ = 0, lcrh_ = 0, cr_ = 0, imsc_ = 0;
  std::function<void(uint8_t)> sink_;
  std::function<void(bool)> irqSink_;
};

}  // namespace ncv
