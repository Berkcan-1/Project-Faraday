#include "ncv_mtk_compat.h"
#include <iomanip>
#include <sstream>

namespace ncv {
void MtkProbeMmioShim::record(char op, uint64_t address, unsigned size, uint64_t value) {
  std::lock_guard<std::mutex> lock(mu_);
  recent_.push_back(Trace{op,address,value,size});
  while (recent_.size()>24) recent_.pop_front();
  switch (op) {
    case 'R': ++reads_; lastRead_ = address; break;
    case 'W': ++writes_; lastWrite_ = address; break;
    case 'r': ++rejectedReads_; lastRejectedRead_ = address; break;
    case 'w': ++rejectedWrites_; lastRejectedWrite_ = address; break;
    default: break;
  }
}
Status MtkProbeMmioShim::read(uint64_t physicalAddress, unsigned size, uint64_t* value) {
  if (!value || !(size==1 || size==2 || size==4 || size==8)) return Status::InvalidArgument;
  const bool inWindow = physicalAddress >= 0x10000000ull &&
                        physicalAddress <= 0x20000000ull - size;
  if (!inWindow) {
    // Keep a trace of every rejected unmapped access too. Previously these
    // returned Unmapped silently, leaving the UI showing reads=0/writes=0.
    record('r', physicalAddress, size, 0);
    return Status::Unmapped;
  }
  *value=0; record('R',physicalAddress,size,*value); return Status::Ok;
}
Status MtkProbeMmioShim::write(uint64_t physicalAddress, unsigned size, uint64_t value) {
  if (!(size==1 || size==2 || size==4 || size==8)) return Status::InvalidArgument;
  const bool inWindow = (physicalAddress < 0x00100000ull) || 
                      (physicalAddress >= 0x10000000ull && physicalAddress <= 0x20000000ull - size);
  if (!inWindow) {
    record('w', physicalAddress, size, value);
    return Status::Unmapped;
  }
  record('W',physicalAddress,size,value); return Status::Ok;
}
std::string MtkProbeMmioShim::report() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::ostringstream out;
  out << "MTK probe fallback active: emulated-window reads=" << reads_ << ", ignored writes=" << writes_
      << ", rejected unmapped reads=" << rejectedReads_ << ", rejected unmapped writes=" << rejectedWrites_;
  if (lastRead_) out << ", last emulated read=0x" << std::hex << lastRead_;
  if (lastWrite_) out << ", last ignored write=0x" << std::hex << lastWrite_;
  if (lastRejectedRead_) out << ", last rejected read=0x" << std::hex << lastRejectedRead_;
  if (lastRejectedWrite_) out << ", last rejected write=0x" << std::hex << lastRejectedWrite_;
  out << "\nRecent unknown physical access trace (oldest first; lowercase r/w = rejected outside fallback aperture):";
  for (const auto& t : recent_) out << "\n  " << t.op << "[" << std::dec << t.size << "] @0x" << std::hex << t.address << " value=0x" << t.value;
  out << "\nModeled NCV board devices: PL011 UART, GICv2 (single CPU; basic APR/NSAPR and DIR registers), ARM generic timer, PL031 RTC, virtio-mmio block, virtio-rng, simple framebuffer, single-vCPU PSCI subset."
      << "\nNot modeled: MT6768-specific clock/reset controllers, PMIC, Mali GPU, camera/audio/modem/Wi-Fi and proprietary vendor HAL behavior.";
  return out.str();
}
void MtkProbeMmioShim::resetCounters() {
  std::lock_guard<std::mutex> lock(mu_);
  reads_=writes_=rejectedReads_=rejectedWrites_=0;
  lastRead_=lastWrite_=lastRejectedRead_=lastRejectedWrite_=0;
  recent_.clear();
}
} // namespace ncv
