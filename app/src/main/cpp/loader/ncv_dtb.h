// Minimal Flattened Device Tree (FDT) builder for the NCV single-vCPU ARM64 board.
// Emits a version-17 DTB for Linux direct boot; it is not a general DT compiler.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../core/ncv_types.h"

namespace ncv {

struct LinuxDtbConfig {
  uint64_t ramBase = 0x40000000ull;
  uint64_t ramSize = 16ull << 20;
  uint64_t uartBase = 0x09000000ull;
  uint64_t gicBase = 0x08000000ull;
  uint32_t counterFrequency = 1000000;
  uint32_t virtioBlockCount = 0;
  uint64_t virtioBlockBase = 0x0A000000ull;
  uint64_t virtioRngBase = 0x0A002000ull;
  uint64_t framebufferBase = 0x3F000000ull;
  uint32_t framebufferWidth = 640, framebufferHeight = 480, framebufferStride = 2560;
  uint64_t initrdBase = 0;
  uint64_t initrdSize = 0;
  // Optional reserved range added to a hybrid Android DTB reservation map.
  // Used for the VM-provided entry stack/scratch area immediately below DTB.
  uint64_t bootScratchReserveBase = 0;
  uint64_t bootScratchReserveSize = 0;
  std::string bootargs = "console=ttyAMA0,115200 earlycon=pl011,mmio32,0x09000000 lpj=10000 androidboot.hardware=ncv androidboot.fstab_suffix=ncv init=/init root=/dev/ram0 rw loglevel=8";
};

// Returns a self-contained big-endian FDT blob. The caller must place it in guest RAM
// and pass its physical address in X0 when entering the Linux Image at EL1.
Status buildLinuxDtb(const LinuxDtbConfig& config, std::vector<uint8_t>* out);

// Adapts a legacy boot.img embedded DTB (raw FDT, concatenated FDT, or Android
// DT table's first entry) for the NCV virtual board. The original SoC nodes are
// retained as probes; RAM, chosen/bootargs, GIC, PL011, timer and virtio-mmio
// descriptions are rewritten/added for NCV. This is experimental for vendor DTBs.
Status buildHybridAndroidDtb(const uint8_t* source, uint64_t sourceSize,
                             const LinuxDtbConfig& config,
                             std::vector<uint8_t>* out, std::string* report = nullptr);

} // namespace ncv
