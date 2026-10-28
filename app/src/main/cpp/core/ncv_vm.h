// NCV - VM nesnesi ve durum makinesi. Android'e bağımlı DEĞİL.
//
// Durumlar:  Created -> Running <-> Paused -> Stopped | Faulted ;  Stopped/Faulted -> (reset) -> Created
// CPU ayrı bir host thread'inde çalışır (UI thread'ini bloklamaz). pause() döndüğünde CPU thread'i
// artık komut yürütmüyor (slice sınırında durmuş) -> CPU state/guest RAM güvenle okunabilir.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../cpu/ncv_cpu.h"
#include "../devices/ncv_uart.h"
#include "../devices/ncv_gicv2.h"
#include "../devices/ncv_virtio_blk.h"
#include "../devices/ncv_virtio_rng.h"
#include "../devices/ncv_pl031_rtc.h"
#include "../devices/ncv_framebuffer.h"
#include "../devices/ncv_mtk_compat.h"
#include "../memory/ncv_bus.h"
#include "../loader/ncv_dtb.h"
#include "../loader/ncv_linux_image.h"
#include "../loader/ncv_android_boot_image.h"
#include "ncv_types.h"

namespace ncv {

struct AndroidBootLoadInfo {
  uint64_t entry = 0;
  uint64_t initrdAddress = 0;
  uint64_t initrdSize = 0;
  uint32_t headerVersion = 0;
  uint64_t selectedDtbSize = 0;
  uint64_t bootDtbSize = 0;
  uint64_t vendorDtbSize = 0;
  bool selectedDtbFromVendorBoot = false;
  uint64_t kernelHeaderAddress = 0;
  uint64_t ramdiskHeaderAddress = 0;
  uint64_t tagsHeaderAddress = 0;
  uint64_t dtbHeaderAddress = 0;
  std::string commandLine;
};

struct BoardConfig {
  uint64_t ramBase = 0x40000000ull;
  uint64_t ramSize = 16ull << 20;     // 16 MiB varsayılan
  uint64_t uartBase = 0x09000000ull;
  uint64_t gicBase = 0x08000000ull;
  uint64_t maxRamBytes = 2ull << 30;
  uint8_t initialEL = 0;                // 0 or 1; EL2/EL3 boot paths are not implemented
  uint8_t compatibilityProfileId = 0;   // 0=strict NCV virtual board, 1=experimental MTK probe fallback
};

enum class VmState : int32_t { Created = 0, Running, Paused, Stopped, Faulted };
const char* vmStateName(VmState s);

class Vm {
 public:
  static std::unique_ptr<Vm> create(const BoardConfig& cfg, Status* status);
  ~Vm();
  Vm(const Vm&) = delete;
  Vm& operator=(const Vm&) = delete;

  VmState state() const;

  // Yalnızca Created durumunda.
  Status loadPayload(GuestAddr addr, const uint8_t* data, uint64_t len);
  Status setEntry(GuestAddr pc);
  // Loads a raw or gzip-compressed ARM64 Linux Image and selects EL1 direct-boot state.
  // Gzip expansion is bounded. Android boot images use loadAndroidBootBundle().
  Status loadLinuxImage(const uint8_t* data, uint64_t len, GuestAddr* outEntry = nullptr);
  // Extracts boot.img, optional required vendor_boot.img and optional init_boot.img;
  // loads the kernel and combined ramdisk/bootconfig into guest RAM. Uses NCV virtual-board DTB.
  Status loadAndroidBootBundle(const uint8_t* bootData, uint64_t bootSize,
                               const uint8_t* vendorData, uint64_t vendorSize,
                               const uint8_t* initBootData, uint64_t initBootSize,
                               AndroidBootLoadInfo* outInfo);
  // Attaches a seekable image file as a modern virtio-mmio block device. The VM takes a dup.
  Status attachBlockFd(int fd, bool readOnly, uint64_t* outSectors = nullptr,
                       const std::string& label = "disk");
  Status attachSuperContainerFd(int fd, const std::string& label = "super");
  Status attachLogicalPartitionFd(int fd, uint64_t logicalSizeBytes,
                                  const std::vector<LogicalBlockExtent>& extents,
                                  const std::string& label);
  size_t blockDeviceCount() const;
  std::string blockDevicesDiagnostics() const;
  std::vector<uint32_t> framebufferArgb() const { return framebuffer_.snapshotArgb(); }
  std::string compatibilityReport() const;
  // Live MTK MMIO probe counters/trace, intended to be sampled while the guest is running.
  std::string mmioProbeReport() const;
  // Builds and loads a minimal ARM64 Linux DTB near the top of guest RAM, selects EL1
  // direct-boot state and passes the DTB physical address in X0 at the next start/runSync.
  std::string loadedBootArgs() const;
  // Lock-free snapshot updated after each guest execution slice; safe to read while RUNNING.
  std::string runtimeDiagnostics() const;
  Status prepareLinuxBootDtb(const std::string& bootargs, GuestAddr* outAddr,
                             uint64_t initrdBase = 0, uint64_t initrdSize = 0);
  // MTK probe mode only: adapt a boot.img or vendor_boot.img DTB to NCV RAM,
  // GICv2, PL011, timers and virtio devices while retaining other vendor nodes as probes.
  Status prepareAndroidBootDtb(const std::string& bootargs, GuestAddr* outAddr,
                               uint64_t initrdBase = 0, uint64_t initrdSize = 0);

  Status start();   // Created -> Running (CPU thread başlatır)
  Status pause();   // Running -> Paused (CPU durana kadar bekler)
  Status resume();  // Paused -> Running
  Status stop();    // Running/Paused -> Stopped; Stopped/Faulted için thread'i toplar (idempotent)
  Status reset();   // Stopped/Faulted -> Created (RAM sıfırlanır, payload yeniden yüklenmeli)

  // Thread'siz, deterministik çalıştırma (test/headless). Created veya Paused(sync) durumundan.
  // InstructionLimit -> Paused; terminal sebep -> Stopped/Faulted.
  StopReason runSync(uint64_t maxInstructions);

  StopInfo lastStop() const;
  std::string consoleOutput() const { return uart_.peekOutput(); }
  std::string takeConsoleOutput() { return uart_.takeOutput(); }
  bool pushConsoleInput(uint8_t b) { return uart_.pushRx(b); }

  // Running durumunda InvalidState döner.
  Status cpuState(CpuState* out) const;
  uint64_t instructionsRetired() const;
  Bus& bus() { return bus_; }  // yalnızca Running değilken kullan
  const BoardConfig& config() const { return cfg_; }

 private:
  explicit Vm(const BoardConfig& cfg);
  Status init();
  void threadMain();
  void joinThread();

  static constexpr uint64_t kSliceInstructions = 10000;

  BoardConfig cfg_;
  Bus bus_;
  Pl011Uart uart_;
  Pl031Rtc rtc_;
  GicV2 gic_;
  GuestFramebuffer framebuffer_;
  VirtioMmioRng rng_;
  MtkProbeMmioShim mtkProbeShim_;
  std::vector<std::unique_ptr<VirtioMmioBlock>> blockDevices_;
  Cpu cpu_;

  mutable std::mutex mu_;
  std::condition_variable cv_;      // CPU thread'ini uyandırır
  std::condition_variable cvIdle_;  // inSlice_ değişimini haber verir
  VmState state_ = VmState::Created;
  bool stopRequested_ = false;
  bool inSlice_ = false;
  bool syncUsed_ = false;
  StopInfo lastStop_;
  uint64_t entry_;
  uint64_t sp_;
  uint64_t bootArg0_ = 0;
  Arm64LinuxImageInfo linuxImageInfo_{};
  bool linuxImageLoaded_ = false;
  uint64_t loadedInitrdBase_ = 0;
  uint64_t loadedInitrdSize_ = 0;
  std::vector<uint8_t> loadedBootDtb_;
  std::string loadedDtbReport_;
  std::string loadedBootArgs_;
  std::thread thread_;
  std::atomic<uint64_t> runtimePc_{0};
  std::atomic<uint64_t> runtimeInstructions_{0};
  std::atomic<uint64_t> runtimeSlices_{0};
  std::atomic<uint32_t> runtimeLastInstruction_{0};
  std::atomic<uint32_t> runtimeSamePcSlices_{0};
  std::atomic<uint64_t> runtimeSamePc_{0};
  std::atomic<uint64_t> runtimeX0_{0}, runtimeX1_{0}, runtimeX2_{0}, runtimeX3_{0};
  std::atomic<uint64_t> runtimeSctlr_{0}, runtimeEsr_{0}, runtimeFar_{0};
  std::atomic<uint64_t> runtimeElr_{0}, runtimeSpsr_{0};
  std::atomic<uint64_t> runtimeLastAbortPc_{0}, runtimeLastAbortVa_{0}, runtimeLastAbortPa_{0};
  std::atomic<uint32_t> runtimeLastAbortInsn_{0};
  std::atomic<int32_t> runtimeLastAbortBusStatus_{static_cast<int32_t>(Status::Ok)};
  std::atomic<uint32_t> runtimeLastAbortSize_{0}, runtimeLastAbortAccess_{255};
  std::atomic<bool> runtimeLastAbortTableWalk_{false};
  std::atomic<uint64_t> runtimeLastAbortTcr_{0}, runtimeLastAbortTtbr0_{0}, runtimeLastAbortTtbr1_{0};
  std::atomic<uint64_t> runtimeLastAbortX8_{0}, runtimeLastAbortWalkRoot_{0}, runtimeLastAbortWalkVaPage_{0};
  std::atomic<uint64_t> runtimeLastAbortL1Desc_{0}, runtimeLastAbortL2Desc_{0}, runtimeLastAbortL3Desc_{0};
  std::atomic<uint64_t> runtimeLastAbortL3EntryPa_{0};
  std::atomic<bool> runtimeLastAbortPteWriterFound_{false};
  std::atomic<uint64_t> runtimeLastAbortPteWriterPc_{0}, runtimeLastAbortPteWriterVa_{0};
  std::atomic<uint64_t> runtimeLastAbortPteWriterPa_{0}, runtimeLastAbortPteWriterValue_{0};
  std::atomic<uint32_t> runtimeLastAbortPteWriterInsn_{0}, runtimeLastAbortPteWriterSize_{0};
  std::atomic<bool> runtimeLastAbortWalkTtbr1_{false}, runtimeLastAbortTlbHit_{false};
  std::atomic<bool> runtimeLastAbortFreshWalkAttempted_{false}, runtimeLastAbortFreshWalkOk_{false};
  std::atomic<uint64_t> runtimeLastAbortFreshPa_{0}, runtimeLastAbortFreshRoot_{0};
  std::atomic<uint64_t> runtimeLastAbortFreshL1Desc_{0}, runtimeLastAbortFreshL2Desc_{0}, runtimeLastAbortFreshL3Desc_{0};
  std::atomic<uint32_t> runtimeLastAbortFreshFsc_{0};
  std::atomic<bool> runtimeLastAbortFreshTableWalk_{false};
  std::atomic<uint64_t> runtimeLastAbortFreshFaultPa_{0};
  std::atomic<int32_t> runtimeLastAbortFreshBusStatus_{static_cast<int32_t>(Status::Ok)};
  std::atomic<uint32_t> runtimeCurrentEl_{0};
};

}  // namespace ncv
