// NCV minimal virtio-mmio block device. Supports modern virtio-mmio v2, one queue, and read-only/raw or Android sparse images.
#pragma once
#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <sys/types.h>
#include "../memory/ncv_bus.h"

namespace ncv {

// Host-side read-only linear view of one liblp logical partition. This maps byte
// ranges in a backing super image; it is NOT a guest Linux device-mapper target.
struct LogicalBlockExtent {
  uint64_t logicalOffsetBytes = 0;
  uint64_t lengthBytes = 0;
  bool zero = false;
  uint32_t sourceDeviceIndex = 0;
  uint64_t physicalOffsetBytes = 0;
};

class VirtioMmioBlock final : public Device {
 public:
  static constexpr uint64_t kRegionSize = 0x200;
  static constexpr uint32_t kMaxQueueSize = 128;
  static Status openFd(Bus* bus, int fd, bool readOnly, std::unique_ptr<VirtioMmioBlock>* out);
  // Wraps a real super partition image in a minimal synthetic GPT carrier disk.
  // The guest sees a GPT partition named "super"; Android init still has to
  // discover it and create dm-linear targets itself.
  static Status openSuperContainerFd(Bus* bus, int fd, const std::string& label,
                                     std::unique_ptr<VirtioMmioBlock>* out);
  static Status openPartitionView(Bus* bus, int fd, uint64_t logicalSizeBytes,
                                  const std::vector<LogicalBlockExtent>& extents,
                                  const std::string& label,
                                  std::unique_ptr<VirtioMmioBlock>* out);
  ~VirtioMmioBlock() override;
  VirtioMmioBlock(const VirtioMmioBlock&) = delete;
  VirtioMmioBlock& operator=(const VirtioMmioBlock&) = delete;

  const char* name() const override { return "virtio-mmio-block"; }
  Status read(uint64_t offset, unsigned size, uint64_t* value) override;
  Status write(uint64_t offset, unsigned size, uint64_t value) override;
  void setIrqSink(std::function<void(bool)> sink);
  void reset();
  uint64_t capacitySectors() const { return logicalSize_ / 512; }
  bool readOnly() const { return readOnly_; }
  bool sparseImage() const { return sparse_; }
  bool isPartitionView() const { return partitionView_; }
  bool isSuperContainer() const { return superContainer_; }
  void setLabel(std::string label) { label_ = std::move(label); }
  const std::string& label() const { return label_; }
  std::string diagnostics() const;
  // Read-only host inspection API; guest I/O still goes through virtio queues.
  Status readLogicalRange(uint64_t offset, uint8_t* out, uint64_t size) { return readDisk(offset, out, size); }

 private:
  struct SparseChunk { uint64_t logicalOffset=0, size=0, fileOffset=0; uint32_t type=0; uint8_t fill[4]{}; };
  VirtioMmioBlock(Bus* bus, int fd, uint64_t fileSize, bool readOnly);
  Status initImage();
  Status readDisk(uint64_t offset, uint8_t* out, uint64_t size);
  Status readSourceDisk(uint64_t offset, uint8_t* out, uint64_t size);
  Status readSuperContainer(uint64_t offset, uint8_t* out, uint64_t size);
  Status initializeSuperGpt();
  Status writeDisk(uint64_t offset, const uint8_t* in, uint64_t size);
  Status processQueue();
  Status readGuest(uint64_t address, void* out, size_t size);
  Status writeGuest(uint64_t address, const void* in, size_t size);
  void updateIrq();
  uint32_t selectedFeatures() const;

  Bus* bus_ = nullptr;
  int fd_ = -1;
  uint64_t fileSize_ = 0;
  uint64_t logicalSize_ = 0;
  uint64_t sourceLogicalSize_ = 0;
  bool readOnly_ = true;
  bool partitionView_ = false;
  bool superContainer_ = false;
  uint64_t superPartitionStartBytes_ = 2048ull * 512ull;
  uint64_t superDiskSectors_ = 0;
  std::array<uint8_t, 512> gptMbr_{};
  std::array<uint8_t, 512> gptPrimaryHeader_{};
  std::array<uint8_t, 512> gptBackupHeader_{};
  std::vector<uint8_t> gptEntries_;
  std::vector<LogicalBlockExtent> logicalExtents_;
  std::string label_ = "disk";
  std::atomic<uint64_t> mmioReads_{0};
  std::atomic<uint64_t> mmioWrites_{0};
  std::atomic<uint64_t> readRequests_{0};
  std::atomic<uint64_t> writeRequests_{0};
  std::atomic<uint64_t> failedRequests_{0};
  bool sparse_ = false;
  uint32_t sparseBlockSize_ = 0;
  std::vector<SparseChunk> sparseChunks_;
  std::function<void(bool)> irqSink_;

  uint32_t deviceFeaturesSel_ = 0, driverFeaturesSel_ = 0, queueSel_ = 0;
  uint32_t driverFeatures_[2]{};
  uint32_t status_ = 0, interruptStatus_ = 0;
  uint32_t queueNum_ = 0, queueReady_ = 0;
  uint64_t queueDesc_ = 0, queueAvail_ = 0, queueUsed_ = 0;
  uint16_t lastAvailIndex_ = 0;
  uint32_t configGeneration_ = 0;
};

} // namespace ncv
