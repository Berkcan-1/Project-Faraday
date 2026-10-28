#include "ncv_android_boot_image.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <sstream>

namespace ncv {
namespace {
constexpr uint64_t kBootMagicSize = 8;
constexpr uint64_t kLegacyHeaderSize = 1632;
constexpr uint64_t kHeaderV1Size = 1648;
constexpr uint64_t kHeaderV2Size = 1660;
constexpr uint64_t kHeaderV3Size = 1580;
constexpr uint64_t kHeaderV4Size = 1584;
constexpr uint64_t kVendorV3Size = 2112;
constexpr uint64_t kVendorV4Size = 2128;
constexpr uint64_t kVendorRamdiskEntryV4MinSize = 108;

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
      (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t le64(const uint8_t* p) { return static_cast<uint64_t>(le32(p)) | (static_cast<uint64_t>(le32(p + 4)) << 32); }
bool addOverflow(uint64_t a, uint64_t b, uint64_t* out) {
  if (a > UINT64_MAX - b) return true;
  *out = a + b;
  return false;
}
bool alignUp(uint64_t value, uint64_t alignment, uint64_t* out) {
  if (!alignment || (alignment & (alignment - 1)) != 0) return false;
  const uint64_t extra = (alignment - (value & (alignment - 1))) & (alignment - 1);
  return !addOverflow(value, extra, out);
}
bool validSlice(uint64_t imageSize, uint64_t offset, uint64_t size) {
  return offset <= imageSize && size <= imageSize - offset;
}
std::string cString(const uint8_t* p, size_t n) {
  size_t len = 0;
  while (len < n && p[len] != 0) ++len;
  std::string out(reinterpret_cast<const char*>(p), len);
  for (char& c : out) if (static_cast<unsigned char>(c) < 0x20 && c != '\t') c = ' ';
  return out;
}
bool validPageSize(uint32_t p) { return p >= 2048 && p <= 65536 && (p & (p - 1)) == 0; }
Status copySlice(const uint8_t* src, uint64_t total, uint64_t offset, uint64_t size, std::vector<uint8_t>* out) {
  if (!out || !validSlice(total, offset, size) || size > static_cast<uint64_t>(SIZE_MAX)) return Status::InvalidArgument;
  try { out->assign(src + static_cast<size_t>(offset), src + static_cast<size_t>(offset + size)); }
  catch (const std::bad_alloc&) { return Status::OutOfMemory; }
  catch (...) { return Status::OutOfMemory; }
  return Status::Ok;
}
}

Status inspectAndroidBootImage(const uint8_t* data, uint64_t size, AndroidBootImageInfo* out) {
  if (!data || !out || size < kLegacyHeaderSize || std::memcmp(data, "ANDROID!", kBootMagicSize) != 0)
    return Status::InvalidArgument;

  AndroidBootImageInfo i;
  i.headerVersion = le32(data + 40);
  if (i.headerVersion > 4) return Status::InvalidArgument;

  uint64_t cursor = 0;
  if (i.headerVersion <= 2) {
    i.pageSize = le32(data + 36);
    if (!validPageSize(i.pageSize)) return Status::InvalidArgument;
    if (size < (i.headerVersion == 0 ? kLegacyHeaderSize : i.headerVersion == 1 ? kHeaderV1Size : kHeaderV2Size))
      return Status::InvalidArgument;
    const uint32_t declaredHeaderSize = i.headerVersion >= 1 ? le32(data + 1644) : static_cast<uint32_t>(kLegacyHeaderSize);
    const uint64_t minimumHeader = i.headerVersion == 0 ? kLegacyHeaderSize : i.headerVersion == 1 ? kHeaderV1Size : kHeaderV2Size;
    if (i.headerVersion >= 1 && (declaredHeaderSize < minimumHeader || declaredHeaderSize > i.pageSize))
      return Status::InvalidArgument;
    i.kernelSize = le32(data + 8);
    i.kernelHeaderAddress = le32(data + 12);
    i.ramdiskSize = le32(data + 16);
    i.ramdiskHeaderAddress = le32(data + 20);
    const uint64_t secondSize = le32(data + 24);
    i.tagsHeaderAddress = le32(data + 32);
    if (i.kernelSize == 0 || i.ramdiskSize == 0) return Status::InvalidArgument;
    cursor = i.pageSize;
    i.kernelOffset = cursor;
    if (!validSlice(size, cursor, i.kernelSize) || alignUp(cursor + i.kernelSize, i.pageSize, &cursor) == false)
      return Status::OutOfBounds;
    i.ramdiskOffset = cursor;
    if (!validSlice(size, cursor, i.ramdiskSize) || alignUp(cursor + i.ramdiskSize, i.pageSize, &cursor) == false)
      return Status::OutOfBounds;
    if (secondSize) {
      if (!validSlice(size, cursor, secondSize) || alignUp(cursor + secondSize, i.pageSize, &cursor) == false)
        return Status::OutOfBounds;
    }
    if (i.headerVersion >= 1) {
      const uint64_t recoverySize = le32(data + 1632);
      const uint64_t recoveryOffset = le64(data + 1636);
      if (recoverySize) {
        const uint64_t off = recoveryOffset ? recoveryOffset : cursor;
        if (!validSlice(size, off, recoverySize) || addOverflow(off, recoverySize, &cursor)) return Status::OutOfBounds;
        if (!alignUp(cursor, i.pageSize, &cursor)) return Status::OutOfBounds;
      }
    }
    if (i.headerVersion >= 2) {
      i.dtbSize = le32(data + 1648);
      i.dtbHeaderAddress = le64(data + 1652);
      if (i.dtbSize) {
        i.dtbOffset = cursor;
        if (!validSlice(size, i.dtbOffset, i.dtbSize)) return Status::OutOfBounds;
      }
    }
    i.commandLine = cString(data + 64, 512);
    const std::string extra = cString(data + 608, 1024);
    if (!extra.empty()) { if (!i.commandLine.empty()) i.commandLine += ' '; i.commandLine += extra; }
  } else {
    i.pageSize = 4096;
    if (size < i.pageSize) return Status::InvalidArgument;
    const uint32_t headerSize = le32(data + 20);
    const uint64_t minimumHeader = i.headerVersion == 3 ? kHeaderV3Size : kHeaderV4Size;
    if (headerSize < minimumHeader || headerSize > i.pageSize) return Status::InvalidArgument;
    i.kernelSize = le32(data + 8);
    i.ramdiskSize = le32(data + 12);
    if (i.kernelSize == 0 && i.ramdiskSize == 0) return Status::InvalidArgument;
    i.kernelOffset = i.pageSize;
    if (!validSlice(size, i.kernelOffset, i.kernelSize) ||
        !alignUp(i.kernelOffset + i.kernelSize, i.pageSize, &cursor)) return Status::OutOfBounds;
    i.ramdiskOffset = cursor;
    if (!validSlice(size, i.ramdiskOffset, i.ramdiskSize)) return Status::OutOfBounds;
    cursor = i.ramdiskOffset + i.ramdiskSize;
    if (i.headerVersion == 4) {
      const uint64_t signatureSize = le32(data + 1580);
      if (!alignUp(cursor, i.pageSize, &cursor) || !validSlice(size, cursor, signatureSize)) return Status::OutOfBounds;
    }
    i.commandLine = cString(data + 44, 1536);
    // GKI boot.img has a kernel. Android 13 launch-device init_boot.img is a
    // ramdisk-only v4 container, so it must not trigger the vendor_boot requirement.
    i.requiresVendorBoot = i.kernelSize != 0 && i.headerVersion >= 3;
  }
  if (i.kernelSize > size || i.ramdiskSize > size) return Status::OutOfBounds;
  *out = std::move(i);
  return Status::Ok;
}

Status inspectVendorBootImage(const uint8_t* data, uint64_t size, AndroidVendorBootImageInfo* out) {
  if (!data || !out || size < kVendorV3Size || std::memcmp(data, "VNDRBOOT", kBootMagicSize) != 0)
    return Status::InvalidArgument;
  AndroidVendorBootImageInfo i;
  i.headerVersion = le32(data + 8);
  if (i.headerVersion != 3 && i.headerVersion != 4) return Status::InvalidArgument;
  i.pageSize = le32(data + 12);
  if (!validPageSize(i.pageSize)) return Status::InvalidArgument;
  const uint32_t headerSize = le32(data + 2096);
  const uint64_t minHeader = i.headerVersion == 3 ? kVendorV3Size : kVendorV4Size;
  if (headerSize < minHeader || headerSize > i.pageSize || size < headerSize) return Status::InvalidArgument;
  i.vendorRamdiskSize = le32(data + 24);
  i.dtbSize = le32(data + 2100);
  i.commandLine = cString(data + 28, 2048);

  uint64_t cursor = 0;
  if (!alignUp(headerSize, i.pageSize, &cursor)) return Status::InvalidArgument;
  i.vendorRamdiskOffset = cursor;
  if (!validSlice(size, cursor, i.vendorRamdiskSize) || addOverflow(cursor, i.vendorRamdiskSize, &cursor) ||
      !alignUp(cursor, i.pageSize, &cursor)) return Status::OutOfBounds;
  i.dtbOffset = cursor;
  if (!validSlice(size, i.dtbOffset, i.dtbSize) || addOverflow(cursor, i.dtbSize, &cursor) ||
      !alignUp(cursor, i.pageSize, &cursor)) return Status::OutOfBounds;

  if (i.headerVersion == 4) {
    i.ramdiskTableSize = le32(data + 2112);
    i.ramdiskTableEntryCount = le32(data + 2116);
    i.ramdiskTableEntrySize = le32(data + 2120);
    i.bootConfigSize = le32(data + 2124);
    if (i.ramdiskTableEntryCount > 0 && i.ramdiskTableEntrySize < kVendorRamdiskEntryV4MinSize)
      return Status::InvalidArgument;
    if (i.ramdiskTableEntryCount != 0 &&
        static_cast<uint64_t>(i.ramdiskTableEntryCount) > UINT64_MAX / i.ramdiskTableEntrySize)
      return Status::OutOfBounds;
    if (i.ramdiskTableEntryCount != 0 &&
        static_cast<uint64_t>(i.ramdiskTableEntryCount) * i.ramdiskTableEntrySize > i.ramdiskTableSize)
      return Status::InvalidArgument;
    i.ramdiskTableOffset = cursor;
    if (!validSlice(size, i.ramdiskTableOffset, i.ramdiskTableSize) ||
        addOverflow(cursor, i.ramdiskTableSize, &cursor) || !alignUp(cursor, i.pageSize, &cursor))
      return Status::OutOfBounds;
    i.bootConfigOffset = cursor;
    if (!validSlice(size, i.bootConfigOffset, i.bootConfigSize)) return Status::OutOfBounds;
  }
  *out = std::move(i);
  return Status::Ok;
}

Status extractAndroidBootBundle(const uint8_t* bootData, uint64_t bootSize,
                                const uint8_t* vendorData, uint64_t vendorSize,
                                const uint8_t* initBootData, uint64_t initBootSize,
                                AndroidBootBundle* out) {
  if (!out) return Status::InvalidArgument;
  AndroidBootImageInfo boot;
  Status st = inspectAndroidBootImage(bootData, bootSize, &boot);
  if (st != Status::Ok || boot.kernelSize == 0) return st == Status::Ok ? Status::InvalidArgument : st;
  AndroidBootBundle bundle;
  bundle.headerVersion = boot.headerVersion;
  bundle.kernelHeaderAddress = boot.kernelHeaderAddress;
  bundle.ramdiskHeaderAddress = boot.ramdiskHeaderAddress;
  bundle.tagsHeaderAddress = boot.tagsHeaderAddress;
  bundle.dtbHeaderAddress = boot.dtbHeaderAddress;
  bundle.commandLine = boot.commandLine;
  st = copySlice(bootData, bootSize, boot.kernelOffset, boot.kernelSize, &bundle.kernel);
  if (st != Status::Ok) return st;
  if (boot.ramdiskSize) {
    st = copySlice(bootData, bootSize, boot.ramdiskOffset, boot.ramdiskSize, &bundle.genericRamdisk);
    if (st != Status::Ok) return st;
  }
  // Android boot header v2 may contain a DTB after the recovery-dtbo payload.
  // Preserve it rather than silently discarding it: device kernels often require
  // their matching tree, even when NCV later adapts that tree to virtual RAM/devices.
  if (boot.dtbSize) {
    st = copySlice(bootData, bootSize, boot.dtbOffset, boot.dtbSize, &bundle.bootDtb);
    if (st != Status::Ok) return st;
  }

  if (initBootData && initBootSize) {
    AndroidBootImageInfo initBoot;
    st = inspectAndroidBootImage(initBootData, initBootSize, &initBoot);
    if (st != Status::Ok) return st;
    if (initBoot.headerVersion != 4 || initBoot.kernelSize != 0 || initBoot.ramdiskSize == 0 ||
        !bundle.genericRamdisk.empty()) return Status::InvalidArgument;
    st = copySlice(initBootData, initBootSize, initBoot.ramdiskOffset, initBoot.ramdiskSize,
                   &bundle.genericRamdisk);
    if (st != Status::Ok) return st;
  }
  // Android 13 launch-device images may place the generic ramdisk in init_boot.img.
  if (bundle.genericRamdisk.empty()) return Status::InvalidArgument;

  if (vendorData && vendorSize) {
    AndroidVendorBootImageInfo vendor;
    st = inspectVendorBootImage(vendorData, vendorSize, &vendor);
    if (st != Status::Ok) return st;
    if (vendor.headerVersion != boot.headerVersion || !boot.requiresVendorBoot) return Status::InvalidArgument;
    st = copySlice(vendorData, vendorSize, vendor.vendorRamdiskOffset, vendor.vendorRamdiskSize, &bundle.vendorRamdisk);
    if (st != Status::Ok) return st;
    if (vendor.dtbSize) {
      st = copySlice(vendorData, vendorSize, vendor.dtbOffset, vendor.dtbSize, &bundle.vendorDtb);
      if (st != Status::Ok) return st;
    }
    if (vendor.bootConfigSize) {
      st = copySlice(vendorData, vendorSize, vendor.bootConfigOffset, vendor.bootConfigSize, &bundle.bootConfig);
      if (st != Status::Ok) return st;
    }
    if (!vendor.commandLine.empty()) {
      if (!bundle.commandLine.empty()) bundle.commandLine += ' ';
      bundle.commandLine += vendor.commandLine;
    }
    bundle.hasVendorBoot = true;
  }
  *out = std::move(bundle);
  return Status::Ok;
}

Status extractAndroidBootBundle(const uint8_t* bootData, uint64_t bootSize,
                                const uint8_t* vendorData, uint64_t vendorSize,
                                AndroidBootBundle* out) {
  return extractAndroidBootBundle(bootData, bootSize, vendorData, vendorSize, nullptr, 0, out);
}

Status extractAndroidInitramfs(const uint8_t* bootData, uint64_t bootSize,
                               const uint8_t* vendorData, uint64_t vendorSize,
                               const uint8_t* initBootData, uint64_t initBootSize,
                               std::vector<uint8_t>* outInitramfs,
                               std::string* outCommandLine) {
  if (!outInitramfs) return Status::InvalidArgument;
  AndroidBootBundle bundle;
  const Status parsed = extractAndroidBootBundle(bootData, bootSize, vendorData, vendorSize,
                                                  initBootData, initBootSize, &bundle);
  if (parsed != Status::Ok) return parsed;
  if (bundle.genericRamdisk.empty()) return Status::InvalidArgument;

  // Android's initramfs can be made from several concatenated cpio/gzip fragments.
  // Match the existing OEM boot path's ordering: vendor fragment(s), then generic ramdisk.
  constexpr uint64_t kMaxInitramfsBytes = 64ull << 20;
  const uint64_t baseSize = static_cast<uint64_t>(bundle.vendorRamdisk.size()) +
                            static_cast<uint64_t>(bundle.genericRamdisk.size());
  const uint64_t bootConfigTrailerSize = bundle.bootConfig.empty()
      ? 0ull : static_cast<uint64_t>(bundle.bootConfig.size()) + 32ull;
  if (baseSize > kMaxInitramfsBytes || bundle.bootConfig.size() > (16ull << 20) ||
      bootConfigTrailerSize > kMaxInitramfsBytes ||
      baseSize > kMaxInitramfsBytes - bootConfigTrailerSize) {
    return Status::OutOfBounds;
  }

  std::vector<uint8_t> initrd;
  try {
    initrd.reserve(static_cast<size_t>(baseSize + bootConfigTrailerSize));
    initrd.insert(initrd.end(), bundle.vendorRamdisk.begin(), bundle.vendorRamdisk.end());
    initrd.insert(initrd.end(), bundle.genericRamdisk.begin(), bundle.genericRamdisk.end());
    if (!bundle.bootConfig.empty()) {
      // Strip console overrides for the physical handset; retain the other bootconfig entries.
      const std::string text(reinterpret_cast<const char*>(bundle.bootConfig.data()), bundle.bootConfig.size());
      std::istringstream lines(text);
      std::ostringstream filtered;
      std::string line;
      while (std::getline(lines, line)) {
        const size_t equals = line.find('=');
        const std::string key = equals == std::string::npos ? line : line.substr(0, equals);
        const bool physicalConsole = key == "console" || key == "earlycon" || key == "earlyprintk" ||
                                     key == "androidboot.console" || key == "kernel.console" || key == "kernel.earlycon";
        if (!physicalConsole) filtered << line << '\n';
      }
      const std::string config = filtered.str();
      if (!config.empty()) initrd.insert(initrd.end(), config.begin(), config.end());
      while (initrd.size() & 3u) initrd.push_back(0);
      const size_t configStart = static_cast<size_t>(baseSize);
      const size_t configSize = initrd.size() - configStart;
      if (configSize > UINT32_MAX || initrd.size() + 20 > kMaxInitramfsBytes) return Status::OutOfBounds;
      uint32_t checksum = 0;
      for (size_t i = configStart; i < initrd.size(); ++i) checksum += initrd[i];
      const uint32_t size32 = static_cast<uint32_t>(configSize);
      for (unsigned i = 0; i < 4; ++i) initrd.push_back(static_cast<uint8_t>(size32 >> (i * 8)));
      for (unsigned i = 0; i < 4; ++i) initrd.push_back(static_cast<uint8_t>(checksum >> (i * 8)));
      constexpr char kBootConfigMagic[] = "#BOOTCONFIG\n";
      initrd.insert(initrd.end(), kBootConfigMagic, kBootConfigMagic + sizeof(kBootConfigMagic) - 1);
    }
  } catch (const std::bad_alloc&) {
    return Status::OutOfMemory;
  } catch (...) {
    return Status::OutOfMemory;
  }
  if (initrd.empty() || initrd.size() > kMaxInitramfsBytes) return Status::OutOfBounds;
  *outInitramfs = std::move(initrd);
  if (outCommandLine) *outCommandLine = bundle.commandLine;
  return Status::Ok;
}

} // namespace ncv
