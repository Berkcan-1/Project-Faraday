// NCV Android boot image parser. Structural validation only; it does not verify AVB signatures.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../core/ncv_types.h"

namespace ncv {

struct AndroidBootImageInfo {
  uint32_t headerVersion = 0;
  uint32_t pageSize = 0;
  uint64_t kernelOffset = 0;
  uint64_t kernelSize = 0;
  uint64_t ramdiskOffset = 0;
  uint64_t ramdiskSize = 0;
  uint64_t dtbOffset = 0;
  uint64_t dtbSize = 0;
  // Legacy v0-v2 physical load-address fields. These are diagnostics: the loader
  // still applies the ARM64 Image header's text_offset rule and reports mismatch.
  uint64_t kernelHeaderAddress = 0;
  uint64_t ramdiskHeaderAddress = 0;
  uint64_t tagsHeaderAddress = 0;
  uint64_t dtbHeaderAddress = 0;
  uint64_t bootConfigOffset = 0;
  uint64_t bootConfigSize = 0;
  std::string commandLine;
  bool requiresVendorBoot = false;
};

struct AndroidVendorBootImageInfo {
  uint32_t headerVersion = 0;
  uint32_t pageSize = 0;
  uint64_t vendorRamdiskOffset = 0;
  uint64_t vendorRamdiskSize = 0;
  uint64_t dtbOffset = 0;
  uint64_t dtbSize = 0;
  uint64_t ramdiskTableOffset = 0;
  uint64_t ramdiskTableSize = 0;
  uint32_t ramdiskTableEntryCount = 0;
  uint32_t ramdiskTableEntrySize = 0;
  uint64_t bootConfigOffset = 0;
  uint64_t bootConfigSize = 0;
  std::string commandLine;
};

struct AndroidBootBundle {
  uint32_t headerVersion = 0;
  std::vector<uint8_t> kernel;
  std::vector<uint8_t> genericRamdisk;
  std::vector<uint8_t> vendorRamdisk;
  std::vector<uint8_t> vendorDtb;
  std::vector<uint8_t> bootDtb;     // legacy boot.img v2 embedded DTB / DT table, if present
  std::vector<uint8_t> bootConfig;
  uint64_t kernelHeaderAddress = 0;
  uint64_t ramdiskHeaderAddress = 0;
  uint64_t tagsHeaderAddress = 0;
  uint64_t dtbHeaderAddress = 0;
  std::string commandLine;
  bool hasVendorBoot = false;
};

// Supports Android boot image header versions 0-4 and vendor_boot versions 3-4.
// All offsets/sizes are bounds-checked before exposing payload slices.
Status inspectAndroidBootImage(const uint8_t* data, uint64_t size, AndroidBootImageInfo* out);
Status inspectVendorBootImage(const uint8_t* data, uint64_t size, AndroidVendorBootImageInfo* out);
Status extractAndroidBootBundle(const uint8_t* bootData, uint64_t bootSize,
                                const uint8_t* vendorData, uint64_t vendorSize,
                                const uint8_t* initBootData, uint64_t initBootSize,
                                AndroidBootBundle* out);
Status extractAndroidBootBundle(const uint8_t* bootData, uint64_t bootSize,
                                const uint8_t* vendorData, uint64_t vendorSize,
                                AndroidBootBundle* out);
// Extracts the real Android ramdisk fragments and bootconfig without loading the
// OEM kernel into a VM. Used for the experimental NCV-kernel + Android-ramdisk path.
Status extractAndroidInitramfs(const uint8_t* bootData, uint64_t bootSize,
                               const uint8_t* vendorData, uint64_t vendorSize,
                               const uint8_t* initBootData, uint64_t initBootSize,
                               std::vector<uint8_t>* outInitramfs,
                               std::string* outCommandLine);

} // namespace ncv
