// NCV - guest physical address space: RAM regions + MMIO device routing.
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "../core/ncv_types.h"

namespace ncv {

// Memory-mapped device interface. offset is relative to the region base.
class Device {
 public:
  virtual ~Device() = default;
  virtual const char* name() const = 0;
  virtual Status read(uint64_t offset, unsigned size, uint64_t* value) = 0;
  virtual Status write(uint64_t offset, unsigned size, uint64_t value) = 0;
};

class Bus {
 public:
  static constexpr uint64_t kRamPageSize = 4096;

  // maxTotalRamBytes limits guest-visible RAM capacity. RAM is backed lazily in 4 KiB pages,
  // so exposing a 2 GiB guest address space does not immediately allocate 2 GiB on the host.
  explicit Bus(uint64_t maxTotalRamBytes = 2ull << 30) : maxRam_(maxTotalRamBytes) {}
  Bus(const Bus&) = delete;
  Bus& operator=(const Bus&) = delete;

  Status addRam(GuestAddr base, uint64_t size);
  // Device ownership stays outside Bus; the device must outlive Bus.
  Status addDevice(GuestAddr base, uint64_t size, Device* device);
  // Optional compatibility fallback for otherwise-unmapped physical accesses. Disabled by default.
  // The fallback device must outlive Bus and should only be enabled for diagnostic profiles.
  void setUnmappedAccessFallback(Device* device) { unmappedFallback_ = device; }

  // size: 1, 2, 4 or 8 bytes. Multi-byte accesses are little-endian.
  // An access must fit within one region; accesses spanning regions return OutOfBounds.
  Status read(GuestAddr addr, unsigned size, uint64_t* out);
  Status write(GuestAddr addr, unsigned size, uint64_t value);

  // RAM-only operations (loads to MMIO are rejected). Uncommitted guest RAM reads as zero.
  Status loadBytes(GuestAddr addr, const uint8_t* data, uint64_t len);
  Status readBytes(GuestAddr addr, uint8_t* out, uint64_t len);

  void zeroRam();
  uint64_t totalRamBytes() const { return ramTotal_; }       // guest-visible capacity
  uint64_t residentRamBytes() const { return residentPages_.load(std::memory_order_relaxed) * kRamPageSize; }
  uint64_t residentRamPages() const { return residentPages_.load(std::memory_order_relaxed); }
  size_t regionCount() const { return regions_.size(); }

 private:
  using RamPage = std::array<uint8_t, static_cast<size_t>(kRamPageSize)>;
  struct Region {
    uint64_t base = 0;
    uint64_t size = 0;
    Device* dev = nullptr;  // null means RAM
    // Missing pages are implicitly zero-filled. This allows a large guest RAM map with low host RSS.
    std::vector<std::unique_ptr<RamPage>> ramPages;
  };

  Status checkNoOverlap(uint64_t base, uint64_t size) const;
  Status insertRegion(Region&& r);
  Region* find(uint64_t addr);
  RamPage* findRamPage(Region& region, uint64_t offset);
  const RamPage* findRamPage(const Region& region, uint64_t offset) const;
  RamPage* ensureRamPage(Region& region, uint64_t offset);

  std::vector<Region> regions_;  // sorted by base address
  uint64_t maxRam_;
  uint64_t ramTotal_ = 0;
  std::atomic<uint64_t> residentPages_{0};
  Device* unmappedFallback_ = nullptr;
};

}  // namespace ncv
