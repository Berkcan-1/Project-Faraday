// NCV - ARM64 Linux Image direct loader. Accepts raw/gzip Image, not Android boot.img containers.
#pragma once
#include <cstdint>

#include "../core/ncv_types.h"
#include "../memory/ncv_bus.h"

namespace ncv {

// Metadata describes the decompressed Image payload if input was gzip-compressed.
struct Arm64LinuxImageInfo {
  uint64_t fileSize = 0;
  uint64_t textOffset = 0;
  uint64_t imageSize = 0;       // Effective reserved span used for overlap checks.
  uint64_t flags = 0;
  uint64_t loadAddress = 0;     // Also the ARM64 Image entry point.
  bool headerImageSizeKnown = false;
};

// Validates the standard 64-byte ARM64 Linux Image header and picks a legal address in RAM.
// The guest board's RAM base is rounded up to a 2 MiB boundary before adding text_offset.
Status inspectArm64LinuxImage(const uint8_t* image, uint64_t len, uint64_t ramBase,
                              uint64_t ramSize, Arm64LinuxImageInfo* outInfo);

// Validates and copies a little-endian ARM64 Linux Image into guest RAM. Accepts either a raw
// Image or a single-member gzip stream containing that Image. Android boot.img containers are
// not parsed here. Decompression is capped at 512 MiB to avoid unbounded memory growth.
Status loadArm64LinuxImage(Bus& bus, const uint8_t* image, uint64_t len, uint64_t ramBase,
                           uint64_t ramSize, Arm64LinuxImageInfo* outInfo);

}  // namespace ncv
