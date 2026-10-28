#include "ncv_bus.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <new>

namespace ncv {
namespace {
inline bool validAccessSize(unsigned s) { return s == 1 || s == 2 || s == 4 || s == 8; }
inline uint64_t sizeMask(unsigned s) { return s >= 8 ? ~0ull : ((1ull << (s * 8)) - 1); }
// Does [base, base+size-1] overflow 64-bit arithmetic?
inline bool rangeWraps(uint64_t base, uint64_t size) { return size == 0 || (size - 1) > (~0ull - base); }
inline bool allZero(const uint8_t* data, size_t size) {
  return std::all_of(data, data + size, [](uint8_t value) { return value == 0; });
}
}  // namespace

Status Bus::checkNoOverlap(uint64_t base, uint64_t size) const {
  const uint64_t end = base + (size - 1);
  for (const Region& r : regions_) {
    const uint64_t rEnd = r.base + (r.size - 1);
    if (base <= rEnd && r.base <= end) return Status::Overlap;
  }
  return Status::Ok;
}

Status Bus::insertRegion(Region&& r) {
  auto it = std::lower_bound(regions_.begin(), regions_.end(), r.base,
                             [](const Region& a, uint64_t b) { return a.base < b; });
  try {
    regions_.insert(it, std::move(r));
  } catch (const std::exception&) {
    return Status::OutOfMemory;
  }
  return Status::Ok;
}

Bus::Region* Bus::find(uint64_t addr) {
  auto it = std::upper_bound(regions_.begin(), regions_.end(), addr,
                             [](uint64_t a, const Region& r) { return a < r.base; });
  if (it == regions_.begin()) return nullptr;
  --it;
  return (addr - it->base < it->size) ? &*it : nullptr;
}

Bus::RamPage* Bus::findRamPage(Region& region, uint64_t offset) {
  const uint64_t pageIndex = offset / kRamPageSize;
  if (pageIndex >= region.ramPages.size()) return nullptr;
  return region.ramPages[static_cast<size_t>(pageIndex)].get();
}

const Bus::RamPage* Bus::findRamPage(const Region& region, uint64_t offset) const {
  const uint64_t pageIndex = offset / kRamPageSize;
  if (pageIndex >= region.ramPages.size()) return nullptr;
  return region.ramPages[static_cast<size_t>(pageIndex)].get();
}

Bus::RamPage* Bus::ensureRamPage(Region& region, uint64_t offset) {
  const uint64_t pageIndex = offset / kRamPageSize;
  if (pageIndex >= region.ramPages.size()) return nullptr;
  auto& slot = region.ramPages[static_cast<size_t>(pageIndex)];
  if (slot) return slot.get();
  try {
    auto page = std::make_unique<RamPage>();
    page->fill(0);
    slot = std::move(page);
    residentPages_.fetch_add(1, std::memory_order_relaxed);
    return slot.get();
  } catch (const std::bad_alloc&) {
    return nullptr;
  } catch (const std::exception&) {
    return nullptr;
  }
}

Status Bus::addRam(GuestAddr base, uint64_t size) {
  if (rangeWraps(base.value, size)) return Status::InvalidArgument;
  if (checkNoOverlap(base.value, size) != Status::Ok) return Status::Overlap;
  if (ramTotal_ > maxRam_ || size > maxRam_ - ramTotal_) return Status::OutOfMemory;
  Region r;
  r.base = base.value;
  r.size = size;
  const uint64_t pageCount = size / kRamPageSize + ((size % kRamPageSize) != 0 ? 1ull : 0ull);
  if (pageCount > static_cast<uint64_t>(SIZE_MAX)) return Status::OutOfMemory;
  try {
    r.ramPages.resize(static_cast<size_t>(pageCount));
  } catch (const std::bad_alloc&) {
    return Status::OutOfMemory;
  } catch (const std::exception&) {
    return Status::OutOfMemory;
  }
  const Status st = insertRegion(std::move(r));
  if (st != Status::Ok) return st;
  ramTotal_ += size;
  return Status::Ok;
}

Status Bus::addDevice(GuestAddr base, uint64_t size, Device* device) {
  if (device == nullptr) return Status::InvalidArgument;
  if (rangeWraps(base.value, size)) return Status::InvalidArgument;
  if (checkNoOverlap(base.value, size) != Status::Ok) return Status::Overlap;
  Region r;
  r.base = base.value;
  r.size = size;
  r.dev = device;
  return insertRegion(std::move(r));
}

Status Bus::read(GuestAddr addr, unsigned size, uint64_t* out) {
  if (out == nullptr || !validAccessSize(size)) return Status::InvalidArgument;
  Region* r = find(addr.value);
  if (r == nullptr) {
    if (unmappedFallback_ != nullptr) {
      if (addr.value > UINT64_MAX - (size - 1)) return Status::OutOfBounds;
      uint64_t fallbackValue = 0;
      const Status fallbackStatus = unmappedFallback_->read(addr.value, size, &fallbackValue);
      if (fallbackStatus != Status::Ok) return fallbackStatus;
      *out = fallbackValue & sizeMask(size);
      return Status::Ok;
    }
    return Status::Unmapped;
  }
  const uint64_t off = addr.value - r->base;
  if (size > r->size - off) return Status::OutOfBounds;
  if (r->dev != nullptr) {
    uint64_t v = 0;
    const Status st = r->dev->read(off, size, &v);
    if (st != Status::Ok) return st;
    *out = v & sizeMask(size);
    return Status::Ok;
  }
  uint64_t value = 0;
  const size_t pageOffset = static_cast<size_t>(off % kRamPageSize);
  if (size <= kRamPageSize - pageOffset) {
    // The common case (instruction fetches and aligned loads) touches one page.
    const RamPage* page = findRamPage(*r, off);
    if (page) {
      for (unsigned i = 0; i < size; ++i)
        value |= static_cast<uint64_t>((*page)[pageOffset + i]) << (8 * i);
    }
  } else {
    for (unsigned i = 0; i < size; ++i) {
      const uint64_t byteOffset = off + i;
      const RamPage* page = findRamPage(*r, byteOffset);
      const uint8_t byte = page ? (*page)[static_cast<size_t>(byteOffset % kRamPageSize)] : 0;
      value |= static_cast<uint64_t>(byte) << (8 * i);
    }
  }
  *out = value;
  return Status::Ok;
}

Status Bus::write(GuestAddr addr, unsigned size, uint64_t value) {
  if (!validAccessSize(size)) return Status::InvalidArgument;
  Region* r = find(addr.value);
  if (r == nullptr) {
    if (unmappedFallback_ != nullptr) {
      if (addr.value > UINT64_MAX - (size - 1)) return Status::OutOfBounds;
      return unmappedFallback_->write(addr.value, size, value);
    }
    return Status::Unmapped;
  }
  const uint64_t off = addr.value - r->base;
  if (size > r->size - off) return Status::OutOfBounds;
  value &= sizeMask(size);
  if (r->dev != nullptr) return r->dev->write(off, size, value);

  // Pre-allocate any page that needs a non-zero byte before mutating data, avoiding partial stores
  // when a cross-page write runs out of host memory. Zero writes into absent pages need no backing.
  const size_t pageOffset = static_cast<size_t>(off % kRamPageSize);
  if (size <= kRamPageSize - pageOffset) {
    if (value != 0 && !ensureRamPage(*r, off)) return Status::OutOfMemory;
    if (RamPage* page = findRamPage(*r, off)) {
      for (unsigned i = 0; i < size; ++i)
        (*page)[pageOffset + i] = static_cast<uint8_t>(value >> (8 * i));
    }
    return Status::Ok;
  }
  for (unsigned i = 0; i < size; ++i) {
    const uint8_t byte = static_cast<uint8_t>(value >> (8 * i));
    if (byte == 0) continue;
    if (!ensureRamPage(*r, off + i)) return Status::OutOfMemory;
  }
  for (unsigned i = 0; i < size; ++i) {
    const uint64_t byteOffset = off + i;
    RamPage* page = findRamPage(*r, byteOffset);
    if (page) (*page)[static_cast<size_t>(byteOffset % kRamPageSize)] = static_cast<uint8_t>(value >> (8 * i));
  }
  return Status::Ok;
}

Status Bus::loadBytes(GuestAddr addr, const uint8_t* data, uint64_t len) {
  if (len == 0) return Status::Ok;
  if (data == nullptr) return Status::InvalidArgument;
  Region* r = find(addr.value);
  if (r == nullptr) return Status::Unmapped;
  if (r->dev != nullptr) return Status::InvalidArgument;
  const uint64_t off = addr.value - r->base;
  if (len > r->size - off) return Status::OutOfBounds;

  // Reserve non-zero pages first. Entirely zero input can remain implicitly zero-backed.
  uint64_t consumed = 0;
  while (consumed < len) {
    const uint64_t at = off + consumed;
    const size_t pageOffset = static_cast<size_t>(at % kRamPageSize);
    const uint64_t chunk64 = std::min<uint64_t>(len - consumed, kRamPageSize - pageOffset);
    const size_t chunk = static_cast<size_t>(chunk64);
    if (!allZero(data + static_cast<size_t>(consumed), chunk) && !ensureRamPage(*r, at)) return Status::OutOfMemory;
    consumed += chunk64;
  }

  consumed = 0;
  while (consumed < len) {
    const uint64_t at = off + consumed;
    const size_t pageOffset = static_cast<size_t>(at % kRamPageSize);
    const uint64_t chunk64 = std::min<uint64_t>(len - consumed, kRamPageSize - pageOffset);
    const size_t chunk = static_cast<size_t>(chunk64);
    if (RamPage* page = findRamPage(*r, at)) {
      std::memcpy(page->data() + pageOffset, data + static_cast<size_t>(consumed), chunk);
    }
    consumed += chunk64;
  }
  return Status::Ok;
}

Status Bus::readBytes(GuestAddr addr, uint8_t* out, uint64_t len) {
  if (len == 0) return Status::Ok;
  if (out == nullptr) return Status::InvalidArgument;
  Region* r = find(addr.value);
  if (r == nullptr) return Status::Unmapped;
  if (r->dev != nullptr) return Status::InvalidArgument;
  const uint64_t off = addr.value - r->base;
  if (len > r->size - off) return Status::OutOfBounds;
  if (len > static_cast<uint64_t>(SIZE_MAX)) return Status::OutOfMemory;
  std::memset(out, 0, static_cast<size_t>(len));

  uint64_t consumed = 0;
  while (consumed < len) {
    const uint64_t at = off + consumed;
    const size_t pageOffset = static_cast<size_t>(at % kRamPageSize);
    const uint64_t chunk64 = std::min<uint64_t>(len - consumed, kRamPageSize - pageOffset);
    const size_t chunk = static_cast<size_t>(chunk64);
    if (const RamPage* page = findRamPage(*r, at)) {
      std::memcpy(out + static_cast<size_t>(consumed), page->data() + pageOffset, chunk);
    }
    consumed += chunk64;
  }
  return Status::Ok;
}

void Bus::zeroRam() {
  for (Region& r : regions_) {
    if (r.dev == nullptr) {
      for (auto& page : r.ramPages) page.reset();
    }
  }
  residentPages_.store(0, std::memory_order_relaxed);
}

}  // namespace ncv
