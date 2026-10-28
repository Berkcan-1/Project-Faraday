#include "ncv_virtio_rng.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace ncv {
namespace {
constexpr uint32_t kMagic=0x74726976u, kVersion=2, kDeviceId=4, kVendorId=0x554D4551u;
constexpr uint16_t kDescNext=1u, kDescWrite=2u;
constexpr size_t kDescriptorSize=16, kMaxTransfer=1u<<20;
bool validReadSize(unsigned size, uint64_t off) { return size==4 && (off&3u)==0; }
}

Status VirtioMmioRng::readGuest(uint64_t address, void* out, size_t size) {
  return bus_ ? bus_->readBytes(GuestAddr(address), static_cast<uint8_t*>(out), size) : Status::InvalidState;
}
Status VirtioMmioRng::writeGuest(uint64_t address, const void* in, size_t size) {
  return bus_ ? bus_->loadBytes(GuestAddr(address), static_cast<const uint8_t*>(in), size) : Status::InvalidState;
}
void VirtioMmioRng::updateIrq() {
  std::function<void(bool)> sink; bool level=false;
  { std::lock_guard<std::mutex> lock(mu_); sink=irqSink_; level=(interruptStatus_&1u)!=0; }
  if (sink) sink(level);
}
void VirtioMmioRng::setIrqSink(std::function<void(bool)> sink) {
  { std::lock_guard<std::mutex> lock(mu_); irqSink_=std::move(sink); }
  updateIrq();
}
void VirtioMmioRng::reset() {
  { std::lock_guard<std::mutex> lock(mu_); deviceFeaturesSel_=driverFeaturesSel_=0; driverFeatures_[0]=driverFeatures_[1]=0;
    status_=interruptStatus_=queueNum_=queueReady_=0; queueDesc_=queueAvail_=queueUsed_=0; lastAvailIndex_=usedIndex_=0; }
  updateIrq();
}
Status VirtioMmioRng::read(uint64_t off, unsigned size, uint64_t* value) {
  if (!value || !validReadSize(size,off) || off>=kRegionSize) return Status::DeviceError;
  std::lock_guard<std::mutex> lock(mu_);
  switch(off) {
    case 0x000: *value=kMagic; break; case 0x004: *value=kVersion; break;
    case 0x008: *value=kDeviceId; break; case 0x00C: *value=kVendorId; break;
    case 0x010: *value=(deviceFeaturesSel_ == 1u) ? 1u : 0u; break; // VIRTIO_F_VERSION_1
    case 0x014: *value=deviceFeaturesSel_; break;
    case 0x020: *value=driverFeatures_[driverFeaturesSel_&1u]; break;
    case 0x024: *value=driverFeaturesSel_; break;
    case 0x030: *value=0; break; // single queue only
    case 0x034: *value=kMaxQueueSize; break;
    case 0x038: *value=queueNum_; break;
    case 0x044: *value=queueReady_; break;
    case 0x060: *value=interruptStatus_; break;
    case 0x070: *value=status_; break;
    case 0x080: *value=static_cast<uint32_t>(queueDesc_); break;
    case 0x084: *value=static_cast<uint32_t>(queueDesc_>>32); break;
    case 0x090: *value=static_cast<uint32_t>(queueAvail_); break;
    case 0x094: *value=static_cast<uint32_t>(queueAvail_>>32); break;
    case 0x0A0: *value=static_cast<uint32_t>(queueUsed_); break;
    case 0x0A4: *value=static_cast<uint32_t>(queueUsed_>>32); break;
    default:
      if (off>=0x100 && off<0x200) *value=0; else return Status::DeviceError;
  }
  return Status::Ok;
}
Status VirtioMmioRng::write(uint64_t off, unsigned size, uint64_t value) {
  if (!validReadSize(size,off) || off>=kRegionSize) return Status::DeviceError;
  bool doQueue=false, doUpdate=false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    const uint32_t v=static_cast<uint32_t>(value);
    switch(off) {
      case 0x014: deviceFeaturesSel_=v; break;
      case 0x020: driverFeatures_[driverFeaturesSel_&1u]=v; break;
      case 0x024: driverFeaturesSel_=v; break;
      case 0x030: if (v!=0) return Status::DeviceError; break;
      case 0x038: if (v>kMaxQueueSize || v==0) return Status::InvalidArgument; queueNum_=v; break;
      case 0x044:
        if (v && (queueNum_==0 || queueDesc_==0 || queueAvail_==0 || queueUsed_==0)) return Status::InvalidState;
        queueReady_=v?1u:0u; if (!queueReady_) { lastAvailIndex_=0; usedIndex_=0; } break;
      case 0x050: if (v==0 && queueReady_) doQueue=true; break;
      case 0x064: interruptStatus_ &= ~v; doUpdate=true; break;
      case 0x070:
        status_=v;
        if (v==0) { deviceFeaturesSel_=driverFeaturesSel_=0; driverFeatures_[0]=driverFeatures_[1]=0; interruptStatus_=0; queueNum_=queueReady_=0; queueDesc_=queueAvail_=queueUsed_=0; lastAvailIndex_=usedIndex_=0; }
        doUpdate=true; break;
      case 0x080: queueDesc_=(queueDesc_&0xFFFFFFFF00000000ull)|v; break;
      case 0x084: queueDesc_=(queueDesc_&0xFFFFFFFFull)|(static_cast<uint64_t>(v)<<32); break;
      case 0x090: queueAvail_=(queueAvail_&0xFFFFFFFF00000000ull)|v; break;
      case 0x094: queueAvail_=(queueAvail_&0xFFFFFFFFull)|(static_cast<uint64_t>(v)<<32); break;
      case 0x0A0: queueUsed_=(queueUsed_&0xFFFFFFFF00000000ull)|v; break;
      case 0x0A4: queueUsed_=(queueUsed_&0xFFFFFFFFull)|(static_cast<uint64_t>(v)<<32); break;
      default: if (off<0x100 || off>=0x200) return Status::DeviceError; break;
    }
  }
  if (doQueue) { const Status st=processQueue(); if (st!=Status::Ok) return st; updateIrq(); }
  if (doUpdate) updateIrq();
  return Status::Ok;
}
Status VirtioMmioRng::processQueue() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!bus_ || !queueReady_ || queueNum_==0 || queueNum_>kMaxQueueSize) return Status::InvalidState;
  uint16_t availIndex=0;
  Status st=readGuest(queueAvail_+2,&availIndex,sizeof(availIndex)); if(st!=Status::Ok) return st;
  while(lastAvailIndex_!=availIndex) {
    uint16_t head=0;
    st=readGuest(queueAvail_+4+static_cast<uint64_t>(lastAvailIndex_%queueNum_)*2,&head,sizeof(head)); if(st!=Status::Ok) return st;
    ++lastAvailIndex_;
    if(head>=queueNum_) return Status::DeviceError;
    uint16_t current=head; size_t total=0; bool completed=false;
    for(uint32_t n=0;n<queueNum_;++n) {
      uint8_t raw[kDescriptorSize]{}; st=readGuest(queueDesc_+static_cast<uint64_t>(current)*kDescriptorSize,raw,sizeof(raw)); if(st!=Status::Ok) return st;
      uint64_t addr=0; uint32_t len=0; uint16_t flags=0,next=0;
      std::memcpy(&addr,raw,8); std::memcpy(&len,raw+8,4); std::memcpy(&flags,raw+12,2); std::memcpy(&next,raw+14,2);
      if ((flags&kDescWrite)==0 || len==0 || len>kMaxTransfer-total) return Status::DeviceError;
      std::vector<uint8_t> randomBytes;
      try {
        randomBytes.resize(len);
        std::random_device random;
        uint32_t word=0;
        for(size_t i=0;i<randomBytes.size();++i) {
          if ((i&3u)==0) word=random();
          randomBytes[i]=static_cast<uint8_t>(word >> ((i&3u)*8));
        }
      } catch (...) { return Status::OutOfMemory; }
      st=writeGuest(addr,randomBytes.data(),randomBytes.size()); if(st!=Status::Ok) return st;
      total+=len;
      if ((flags&kDescNext)==0) { completed=true; break; }
      if (next>=queueNum_) return Status::DeviceError;
      current=next;
    }
    if (!completed) return Status::DeviceError;
    uint8_t usedElem[8]{}; const uint32_t id=head, written=static_cast<uint32_t>(total);
    std::memcpy(usedElem,&id,4); std::memcpy(usedElem+4,&written,4);
    st=writeGuest(queueUsed_+4+static_cast<uint64_t>(usedIndex_%queueNum_)*8,usedElem,sizeof(usedElem)); if(st!=Status::Ok) return st;
    ++usedIndex_; st=writeGuest(queueUsed_+2,&usedIndex_,sizeof(usedIndex_)); if(st!=Status::Ok) return st;
    interruptStatus_|=1u;
  }
  // Callback deliberately happens after the device lock is released by the caller's next poll.
  return Status::Ok;
}
} // namespace ncv
