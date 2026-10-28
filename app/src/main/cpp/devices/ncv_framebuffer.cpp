#include "ncv_framebuffer.h"
#include <algorithm>

namespace ncv {
Status GuestFramebuffer::read(uint64_t offset, unsigned size, uint64_t* value) {
  if (!value || !(size==1 || size==2 || size==4 || size==8) || offset > kRegionSize || size > kRegionSize-offset) return Status::OutOfBounds;
  readOps_.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(mu_); uint64_t v=0;
  for (unsigned i=0;i<size;++i) v |= static_cast<uint64_t>(bytes_[static_cast<size_t>(offset+i)]) << (i*8);
  *value=v; return Status::Ok;
}
Status GuestFramebuffer::write(uint64_t offset, unsigned size, uint64_t value) {
  if (!(size==1 || size==2 || size==4 || size==8) || offset > kRegionSize || size > kRegionSize-offset) return Status::OutOfBounds;
  writeOps_.fetch_add(1, std::memory_order_relaxed);
  if (value != 0) nonZeroWriteOps_.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(mu_);
  for(unsigned i=0;i<size;++i) bytes_[static_cast<size_t>(offset+i)]=static_cast<uint8_t>(value>>(i*8));
  return Status::Ok;
}
std::vector<uint32_t> GuestFramebuffer::snapshotArgb() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<uint32_t> out(static_cast<size_t>(kWidth)*kHeight);
  for (size_t i=0, p=0; p<out.size(); ++p, i+=4) {
    const uint32_t b=bytes_[i], g=bytes_[i+1], r=bytes_[i+2];
    out[p]=0xFF000000u | (r<<16) | (g<<8) | b;
  }
  return out;
}
void GuestFramebuffer::clear() { std::lock_guard<std::mutex> lock(mu_); std::fill(bytes_.begin(), bytes_.end(), 0); readOps_.store(0, std::memory_order_relaxed); writeOps_.store(0, std::memory_order_relaxed); nonZeroWriteOps_.store(0, std::memory_order_relaxed); }
} // namespace ncv
