#include "ncv_virtio_blk.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace ncv {
namespace {
constexpr uint32_t kVirtioMagic = 0x74726976u;
constexpr uint32_t kVirtioVersion = 2;
constexpr uint32_t kDeviceBlock = 2;
constexpr uint32_t kVendorNcv = 0x4E435600u;
constexpr uint32_t kStatusDriverOk = 4u;
constexpr uint32_t kStatusFeaturesOk = 8u;
constexpr uint32_t kStatusFailed = 128u;
constexpr uint32_t kDescNext = 1u;
constexpr uint32_t kDescWrite = 2u;
constexpr uint32_t kDescIndirect = 4u;
constexpr uint32_t kReqIn = 0u;
constexpr uint32_t kReqOut = 1u;
constexpr uint32_t kReqFlush = 4u;
constexpr uint32_t kReqGetId = 8u;
constexpr uint8_t kBlkOk = 0;
constexpr uint8_t kBlkIoErr = 1;
constexpr uint8_t kBlkUnsupported = 2;
constexpr uint32_t kSparseMagic = 0xED26FF3Au;
constexpr uint16_t kSparseRaw = 0xCAC1u;
constexpr uint16_t kSparseFill = 0xCAC2u;
constexpr uint16_t kSparseDontCare = 0xCAC3u;
constexpr uint16_t kSparseCrc32 = 0xCAC4u;
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8)); }
uint32_t le32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint64_t le64(const uint8_t* p) { return static_cast<uint64_t>(le32(p)) | (static_cast<uint64_t>(le32(p + 4)) << 32); }
void putLe16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
void putLe32(uint8_t* p, uint32_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); p[2] = static_cast<uint8_t>(v >> 16); p[3] = static_cast<uint8_t>(v >> 24); }
bool addOver(uint64_t a, uint64_t b, uint64_t* out) { if (a > UINT64_MAX - b) return true; *out = a + b; return false; }
void putLe64(uint8_t* p, uint64_t v) { putLe32(p, static_cast<uint32_t>(v)); putLe32(p + 4, static_cast<uint32_t>(v >> 32)); }
uint32_t crc32Ieee(const uint8_t* data, size_t size) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}
void putUtf16LeAscii(uint8_t* dst, const char* value, size_t maxUnits) {
  size_t i = 0;
  for (; value[i] != '\0' && i < maxUnits; ++i) { dst[i * 2] = static_cast<uint8_t>(value[i]); dst[i * 2 + 1] = 0; }
}
}

VirtioMmioBlock::VirtioMmioBlock(Bus* bus, int fd, uint64_t fileSize, bool readOnly)
    : bus_(bus), fd_(fd), fileSize_(fileSize), readOnly_(readOnly) {}

VirtioMmioBlock::~VirtioMmioBlock() { if (fd_ >= 0) close(fd_); }

Status VirtioMmioBlock::openFd(Bus* bus, int fd, bool readOnly, std::unique_ptr<VirtioMmioBlock>* out) {
  if (!bus || fd < 0 || !out) return Status::InvalidArgument;
  struct stat st{};
  if (fstat(fd, &st) != 0 || st.st_size <= 0 || !S_ISREG(st.st_mode)) return Status::IoError;
  const int copy = dup(fd);
  if (copy < 0) return Status::IoError;
  std::unique_ptr<VirtioMmioBlock> device;
  try { device.reset(new VirtioMmioBlock(bus, copy, static_cast<uint64_t>(st.st_size), readOnly)); }
  catch (...) { close(copy); return Status::OutOfMemory; }
  const Status init = device->initImage();
  if (init != Status::Ok) return init;
  *out = std::move(device);
  return Status::Ok;
}

Status VirtioMmioBlock::openSuperContainerFd(Bus* bus, int fd, const std::string& label,
                                                         std::unique_ptr<VirtioMmioBlock>* out) {
  if (!bus || fd < 0 || !out) return Status::InvalidArgument;
  std::unique_ptr<VirtioMmioBlock> device;
  const Status opened = openFd(bus, fd, true, &device);
  if (opened != Status::Ok) return opened;
  if (device->sourceLogicalSize_ == 0 || device->sourceLogicalSize_ % 512ull != 0) return Status::InvalidArgument;
  const Status gpt = device->initializeSuperGpt();
  if (gpt != Status::Ok) return gpt;
  device->superContainer_ = true;
  device->readOnly_ = true;
  device->label_ = label.empty() ? "super-container" : label;
  *out = std::move(device);
  return Status::Ok;
}

Status VirtioMmioBlock::initializeSuperGpt() {
  constexpr uint64_t kStartLba = 2048;  // 1 MiB aligned super partition start.
  constexpr uint64_t kGptEntriesLbaCount = 32; // 128 entries * 128 bytes / 512.
  const uint64_t sourceSectors = sourceLogicalSize_ / 512ull;
  if (sourceSectors == 0 || sourceSectors > UINT64_MAX - kStartLba - 34ull) return Status::OutOfBounds;
  const uint64_t totalSectors = kStartLba + sourceSectors + 34ull;
  if (totalSectors > UINT64_MAX / 512ull) return Status::OutOfBounds;
  superDiskSectors_ = totalSectors;
  superPartitionStartBytes_ = kStartLba * 512ull;
  logicalSize_ = totalSectors * 512ull;

  try { gptEntries_.assign(static_cast<size_t>(kGptEntriesLbaCount * 512ull), 0); }
  catch (...) { return Status::OutOfMemory; }

  // EFI System Partition table entry with PARTNAME="super". The partition type
  // is Basic Data; Android identifies this carrier through the GPT partition name.
  static constexpr uint8_t kBasicDataGuid[16] = {
      0xA2,0xA0,0xD0,0xEB,0xE5,0xB3,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7};
  static constexpr uint8_t kPartitionGuid[16] = {
      0x41,0x7A,0x19,0x2C,0xA4,0x77,0x4B,0x62,0xB1,0x39,0x4E,0x43,0x56,0x53,0x55,0x50};
  static constexpr uint8_t kDiskGuid[16] = {
      0x4E,0x43,0x56,0x2D,0x53,0x55,0x50,0x45,0x52,0x2D,0x47,0x50,0x54,0x2D,0x30,0x31};
  uint8_t* entry = gptEntries_.data();
  std::memcpy(entry, kBasicDataGuid, sizeof(kBasicDataGuid));
  std::memcpy(entry + 16, kPartitionGuid, sizeof(kPartitionGuid));
  putLe64(entry + 32, kStartLba);
  putLe64(entry + 40, kStartLba + sourceSectors - 1ull);
  putUtf16LeAscii(entry + 56, "super", 36);
  const uint32_t entriesCrc = crc32Ieee(gptEntries_.data(), gptEntries_.size());

  // Protective MBR covering the synthesized carrier disk.
  gptMbr_.fill(0);
  uint8_t* mbrEntry = gptMbr_.data() + 446;
  mbrEntry[0] = 0; mbrEntry[1] = 0x00; mbrEntry[2] = 0x02; mbrEntry[3] = 0x00;
  mbrEntry[4] = 0xEE; mbrEntry[5] = 0xFF; mbrEntry[6] = 0xFF; mbrEntry[7] = 0xFF;
  putLe32(mbrEntry + 8, 1);
  putLe32(mbrEntry + 12, static_cast<uint32_t>(std::min<uint64_t>(totalSectors - 1ull, 0xFFFFFFFFull)));
  gptMbr_[510] = 0x55; gptMbr_[511] = 0xAA;

  auto makeHeader = [&](std::array<uint8_t, 512>* header, uint64_t currentLba,
                        uint64_t alternateLba, uint64_t entriesLba) {
    header->fill(0);
    uint8_t* h = header->data();
    std::memcpy(h, "EFI PART", 8);
    putLe32(h + 8, 0x00010000u); // GPT revision 1.0
    putLe32(h + 12, 92u);
    putLe64(h + 24, currentLba);
    putLe64(h + 32, alternateLba);
    putLe64(h + 40, 34ull);
    putLe64(h + 48, totalSectors - 34ull);
    std::memcpy(h + 56, kDiskGuid, sizeof(kDiskGuid));
    putLe64(h + 72, entriesLba);
    putLe32(h + 80, 128u);
    putLe32(h + 84, 128u);
    putLe32(h + 88, entriesCrc);
    putLe32(h + 16, 0);
    putLe32(h + 16, crc32Ieee(h, 92));
  };
  makeHeader(&gptPrimaryHeader_, 1ull, totalSectors - 1ull, 2ull);
  makeHeader(&gptBackupHeader_, totalSectors - 1ull, 1ull, totalSectors - 1ull - kGptEntriesLbaCount);
  return Status::Ok;
}

Status VirtioMmioBlock::openPartitionView(Bus* bus, int fd, uint64_t logicalSizeBytes,
                                                   const std::vector<LogicalBlockExtent>& extents,
                                                   const std::string& label,
                                                   std::unique_ptr<VirtioMmioBlock>* out) {
  if (!bus || fd < 0 || !out || logicalSizeBytes == 0 || logicalSizeBytes % 512 != 0 || extents.empty())
    return Status::InvalidArgument;
  std::unique_ptr<VirtioMmioBlock> device;
  const Status opened = openFd(bus, fd, true, &device);
  if (opened != Status::Ok) return opened;
  const uint64_t sourceSize = device->logicalSize_;
  uint64_t expectedLogical = 0;
  for (const auto& extent : extents) {
    if (extent.lengthBytes == 0 || extent.logicalOffsetBytes != expectedLogical ||
        extent.logicalOffsetBytes > logicalSizeBytes || extent.lengthBytes > logicalSizeBytes - extent.logicalOffsetBytes)
      return Status::InvalidArgument;
    if (extent.sourceDeviceIndex != 0) return Status::InvalidArgument; // external super devices are not attached yet
    if (!extent.zero) {
      if (extent.physicalOffsetBytes > sourceSize || extent.lengthBytes > sourceSize - extent.physicalOffsetBytes)
        return Status::OutOfBounds;
    }
    if (expectedLogical > UINT64_MAX - extent.lengthBytes) return Status::OutOfBounds;
    expectedLogical += extent.lengthBytes;
  }
  if (expectedLogical != logicalSizeBytes) return Status::InvalidArgument;
  try { device->logicalExtents_ = extents; }
  catch (...) { return Status::OutOfMemory; }
  device->sourceLogicalSize_ = sourceSize;
  device->logicalSize_ = logicalSizeBytes;
  device->partitionView_ = true;
  device->readOnly_ = true;
  device->label_ = label.empty() ? "logical-partition" : label;
  *out = std::move(device);
  return Status::Ok;
}

Status VirtioMmioBlock::initImage() {
  if (fileSize_ < 4) return Status::InvalidArgument;
  uint8_t header[28]{};
  const ssize_t got = pread(fd_, header, sizeof(header), 0);
  if (got < 4) return Status::IoError;
  sparse_ = got >= static_cast<ssize_t>(sizeof(header)) && le32(header) == kSparseMagic;
  if (!sparse_) {
    logicalSize_ = fileSize_;
    sourceLogicalSize_ = logicalSize_;
    return logicalSize_ >= 512 ? Status::Ok : Status::InvalidArgument;
  }
  if (readOnly_ == false) return Status::InvalidArgument; // sparse expansion is read-only until a writable overlay is implemented
  const uint16_t major = le16(header + 4), fileHdrSz = le16(header + 8), chunkHdrSz = le16(header + 10);
  sparseBlockSize_ = le32(header + 12);
  const uint32_t totalBlocks = le32(header + 16), chunkCount = le32(header + 20);
  if (major != 1 || fileHdrSz < 28 || fileHdrSz > 4096 || chunkHdrSz < 12 || chunkHdrSz > 1024 ||
      sparseBlockSize_ < 512 || (sparseBlockSize_ % 512) != 0 || totalBlocks == 0 ||
      static_cast<uint64_t>(totalBlocks) > UINT64_MAX / sparseBlockSize_ || chunkCount > 1000000u)
    return Status::InvalidArgument;
  logicalSize_ = static_cast<uint64_t>(totalBlocks) * sparseBlockSize_;
  uint64_t fileOff = fileHdrSz, logicalOff = 0;
  try { sparseChunks_.reserve(chunkCount); } catch (...) { return Status::OutOfMemory; }
  std::array<uint8_t, 1024> chunkHeader{};
  for (uint32_t n = 0; n < chunkCount; ++n) {
    if (fileOff > fileSize_ || chunkHdrSz > fileSize_ - fileOff) return Status::OutOfBounds;
    if (pread(fd_, chunkHeader.data(), chunkHdrSz, static_cast<off_t>(fileOff)) != chunkHdrSz) return Status::IoError;
    const uint16_t type = le16(chunkHeader.data());
    const uint32_t blockCount = le32(chunkHeader.data() + 4);
    const uint32_t totalSize = le32(chunkHeader.data() + 8);
    if (totalSize < chunkHdrSz || totalSize > fileSize_ - fileOff) return Status::OutOfBounds;
    SparseChunk c; c.logicalOffset = logicalOff; c.type = type;
    if (static_cast<uint64_t>(blockCount) > UINT64_MAX / sparseBlockSize_) return Status::OutOfBounds;
    c.size = static_cast<uint64_t>(blockCount) * sparseBlockSize_;
    c.fileOffset = fileOff + chunkHdrSz;
    if (type == kSparseRaw) {
      if (c.size != totalSize - chunkHdrSz) return Status::InvalidArgument;
    } else if (type == kSparseFill) {
      if (totalSize != static_cast<uint32_t>(chunkHdrSz) + 4u || pread(fd_, c.fill, 4, static_cast<off_t>(c.fileOffset)) != 4) return Status::InvalidArgument;
    } else if (type == kSparseDontCare) {
      if (totalSize != chunkHdrSz) return Status::InvalidArgument;
    } else if (type == kSparseCrc32) {
      if (blockCount != 0 || totalSize != static_cast<uint32_t>(chunkHdrSz) + 4u) return Status::InvalidArgument;
      fileOff += totalSize;
      continue;
    } else return Status::InvalidArgument;
    uint64_t nextLogical = 0, nextFile = 0;
    if (addOver(logicalOff, c.size, &nextLogical) || nextLogical > logicalSize_ ||
        addOver(fileOff, totalSize, &nextFile)) return Status::OutOfBounds;
    sparseChunks_.push_back(c);
    logicalOff = nextLogical;
    fileOff = nextFile;
  }
  if (logicalOff != logicalSize_ || fileOff > fileSize_) return Status::InvalidArgument;
  sourceLogicalSize_ = logicalSize_;
  return Status::Ok;
}

void VirtioMmioBlock::setIrqSink(std::function<void(bool)> sink) { irqSink_ = std::move(sink); updateIrq(); }
void VirtioMmioBlock::updateIrq() { if (irqSink_) irqSink_((interruptStatus_ & 1u) != 0); }
void VirtioMmioBlock::reset() {
  deviceFeaturesSel_ = driverFeaturesSel_ = queueSel_ = 0;
  driverFeatures_[0] = driverFeatures_[1] = 0;
  status_ = interruptStatus_ = queueNum_ = queueReady_ = 0;
  queueDesc_ = queueAvail_ = queueUsed_ = 0;
  lastAvailIndex_ = 0; configGeneration_ = 0;
  updateIrq();
}
uint32_t VirtioMmioBlock::selectedFeatures() const {
  if (deviceFeaturesSel_ == 0) {
    uint32_t f = (1u << 1) | (1u << 2) | (1u << 6); // SIZE_MAX, SEG_MAX, BLK_SIZE
    if (readOnly_) f |= 1u << 5;
    else f |= 1u << 9; // FLUSH
    return f;
  }
  if (deviceFeaturesSel_ == 1) return 1u; // VIRTIO_F_VERSION_1 at bit 32
  return 0;
}

Status VirtioMmioBlock::read(uint64_t o, unsigned size, uint64_t* value) {
  mmioReads_.fetch_add(1, std::memory_order_relaxed);
  if (!value || size != 4 || (o & 3u) || o >= kRegionSize) return Status::DeviceError;
  uint32_t v = 0;
  switch (o) {
    case 0x000: v = kVirtioMagic; break;
    case 0x004: v = kVirtioVersion; break;
    case 0x008: v = kDeviceBlock; break;
    case 0x00c: v = kVendorNcv; break;
    case 0x010: v = selectedFeatures(); break;
    case 0x014: v = deviceFeaturesSel_; break;
    case 0x020: v = driverFeatures_[driverFeaturesSel_ < 2 ? driverFeaturesSel_ : 0]; break;
    case 0x024: v = driverFeaturesSel_; break;
    case 0x030: v = queueSel_; break;
    case 0x034: v = queueSel_ == 0 ? kMaxQueueSize : 0; break;
    case 0x038: v = queueNum_; break;
    case 0x044: v = queueReady_; break;
    case 0x060: v = interruptStatus_; break;
    case 0x070: v = status_; break;
    case 0x080: v = static_cast<uint32_t>(queueDesc_); break;
    case 0x084: v = static_cast<uint32_t>(queueDesc_ >> 32); break;
    case 0x090: v = static_cast<uint32_t>(queueAvail_); break;
    case 0x094: v = static_cast<uint32_t>(queueAvail_ >> 32); break;
    case 0x0a0: v = static_cast<uint32_t>(queueUsed_); break;
    case 0x0a4: v = static_cast<uint32_t>(queueUsed_ >> 32); break;
    case 0x0fc: v = configGeneration_; break;
    case 0x100: v = static_cast<uint32_t>(capacitySectors()); break;
    case 0x104: v = static_cast<uint32_t>(capacitySectors() >> 32); break;
    case 0x114: v = 512; break; // blk_size
    case 0x118: v = kMaxQueueSize; break; // seg_max approximation
    default:
      if (o >= 0x100 && o < 0x140) v = 0;
      else return Status::DeviceError;
  }
  *value = v;
  return Status::Ok;
}

Status VirtioMmioBlock::write(uint64_t o, unsigned size, uint64_t value) {
  mmioWrites_.fetch_add(1, std::memory_order_relaxed);
  if (size != 4 || (o & 3u) || o >= kRegionSize) return Status::DeviceError;
  const uint32_t v = static_cast<uint32_t>(value);
  switch (o) {
    case 0x014: deviceFeaturesSel_ = v; return Status::Ok;
    case 0x020: if (driverFeaturesSel_ < 2) driverFeatures_[driverFeaturesSel_] = v; return Status::Ok;
    case 0x024: driverFeaturesSel_ = v; return Status::Ok;
    case 0x030: queueSel_ = v; return Status::Ok;
    case 0x038: if (queueSel_ != 0 || v > kMaxQueueSize || (v && (v & (v - 1u)))) return Status::DeviceError; queueNum_ = v; return Status::Ok;
    case 0x044: if (queueSel_ != 0 || v > 1) return Status::DeviceError; queueReady_ = v; return Status::Ok;
    case 0x050: if (v != 0 || queueSel_ != 0) return Status::DeviceError; return processQueue();
    case 0x064: interruptStatus_ &= ~v; updateIrq(); return Status::Ok;
    case 0x070:
      if (v == 0) { reset(); return Status::Ok; }
      status_ = v;
      if ((status_ & kStatusFeaturesOk) && (driverFeatures_[1] & 1u) == 0) {
        status_ |= kStatusFailed;
        return Status::DeviceError;
      }
      return Status::Ok;
    case 0x080: queueDesc_ = (queueDesc_ & 0xFFFFFFFF00000000ull) | v; return Status::Ok;
    case 0x084: queueDesc_ = (queueDesc_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(v) << 32); return Status::Ok;
    case 0x090: queueAvail_ = (queueAvail_ & 0xFFFFFFFF00000000ull) | v; return Status::Ok;
    case 0x094: queueAvail_ = (queueAvail_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(v) << 32); return Status::Ok;
    case 0x0a0: queueUsed_ = (queueUsed_ & 0xFFFFFFFF00000000ull) | v; return Status::Ok;
    case 0x0a4: queueUsed_ = (queueUsed_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(v) << 32); return Status::Ok;
    default: return Status::DeviceError;
  }
}

Status VirtioMmioBlock::readGuest(uint64_t addr, void* out, size_t size) {
  if (size == 0) return Status::Ok;
  return bus_->readBytes(GuestAddr(addr), static_cast<uint8_t*>(out), size);
}
Status VirtioMmioBlock::writeGuest(uint64_t addr, const void* in, size_t size) {
  if (size == 0) return Status::Ok;
  return bus_->loadBytes(GuestAddr(addr), static_cast<const uint8_t*>(in), size);
}

Status VirtioMmioBlock::readDisk(uint64_t offset, uint8_t* out, uint64_t size) {
  if (!out || offset > logicalSize_ || size > logicalSize_ - offset) return Status::OutOfBounds;
  if (!size) return Status::Ok;
  if (superContainer_) return readSuperContainer(offset, out, size);
  if (!partitionView_) return readSourceDisk(offset, out, size);

  uint64_t logicalPosition = offset;
  uint64_t remaining = size;
  uint64_t destinationOffset = 0;
  while (remaining > 0) {
    const LogicalBlockExtent* found = nullptr;
    // liblp extents are stored in logical order and are validated to cover the partition exactly.
    size_t low = 0, high = logicalExtents_.size();
    while (low < high) {
      const size_t mid = low + (high - low) / 2;
      const auto& extent = logicalExtents_[mid];
      if (logicalPosition < extent.logicalOffsetBytes) high = mid;
      else if (logicalPosition >= extent.logicalOffsetBytes + extent.lengthBytes) low = mid + 1;
      else { found = &extent; break; }
    }
    if (!found) return Status::OutOfBounds;
    const uint64_t within = logicalPosition - found->logicalOffsetBytes;
    const uint64_t count = std::min<uint64_t>(remaining, found->lengthBytes - within);
    if (found->zero) {
      std::memset(out + destinationOffset, 0, static_cast<size_t>(count));
    } else {
      if (found->sourceDeviceIndex != 0 || found->physicalOffsetBytes > sourceLogicalSize_ ||
          within > sourceLogicalSize_ - found->physicalOffsetBytes ||
          count > sourceLogicalSize_ - found->physicalOffsetBytes - within)
        return Status::OutOfBounds;
      const Status readStatus = readSourceDisk(found->physicalOffsetBytes + within,
                                               out + destinationOffset, count);
      if (readStatus != Status::Ok) return readStatus;
    }
    logicalPosition += count;
    destinationOffset += count;
    remaining -= count;
  }
  return Status::Ok;
}

Status VirtioMmioBlock::readSuperContainer(uint64_t offset, uint8_t* out, uint64_t size) {
  if (!superContainer_ || !out || offset > logicalSize_ || size > logicalSize_ - offset || size > SIZE_MAX)
    return Status::OutOfBounds;
  if (!size) return Status::Ok;
  std::memset(out, 0, static_cast<size_t>(size));
  const uint64_t requestEnd = offset + size;
  auto copyRegion = [&](uint64_t regionOffset, const uint8_t* region, uint64_t regionSize) {
    const uint64_t regionEnd = regionOffset + regionSize;
    const uint64_t begin = std::max(offset, regionOffset);
    const uint64_t end = std::min(requestEnd, regionEnd);
    if (begin < end) std::memcpy(out + static_cast<size_t>(begin - offset),
                                 region + static_cast<size_t>(begin - regionOffset),
                                 static_cast<size_t>(end - begin));
  };
  constexpr uint64_t kSector = 512ull;
  constexpr uint64_t kEntryTableBytes = 128ull * 128ull;
  const uint64_t sourceEnd = superPartitionStartBytes_ + sourceLogicalSize_;
  const uint64_t begin = std::max(offset, superPartitionStartBytes_);
  const uint64_t end = std::min(requestEnd, sourceEnd);
  if (begin < end) {
    const Status st = readSourceDisk(begin - superPartitionStartBytes_,
                                     out + static_cast<size_t>(begin - offset), end - begin);
    if (st != Status::Ok) return st;
  }
  copyRegion(0, gptMbr_.data(), gptMbr_.size());
  copyRegion(kSector, gptPrimaryHeader_.data(), gptPrimaryHeader_.size());
  copyRegion(2ull * kSector, gptEntries_.data(), std::min<uint64_t>(gptEntries_.size(), kEntryTableBytes));
  const uint64_t backupEntriesLba = superDiskSectors_ - 1ull - 32ull;
  copyRegion(backupEntriesLba * kSector, gptEntries_.data(), std::min<uint64_t>(gptEntries_.size(), kEntryTableBytes));
  copyRegion((superDiskSectors_ - 1ull) * kSector, gptBackupHeader_.data(), gptBackupHeader_.size());
  return Status::Ok;
}

Status VirtioMmioBlock::readSourceDisk(uint64_t offset, uint8_t* out, uint64_t size) {
  if (!out || offset > sourceLogicalSize_ || size > sourceLogicalSize_ - offset) return Status::OutOfBounds;
  if (!size) return Status::Ok;
  if (!sparse_) {
    uint64_t done = 0;
    while (done < size) {
      const size_t chunk = static_cast<size_t>(std::min<uint64_t>(size - done, 1ull << 20));
      const ssize_t n = pread(fd_, out + done, chunk, static_cast<off_t>(offset + done));
      if (n <= 0) return Status::IoError;
      done += static_cast<uint64_t>(n);
    }
    return Status::Ok;
  }
  std::memset(out, 0, static_cast<size_t>(size));
  const uint64_t requestEnd = offset + size;
  for (const SparseChunk& c : sparseChunks_) {
    const uint64_t cEnd = c.logicalOffset + c.size;
    const uint64_t begin = std::max(offset, c.logicalOffset);
    const uint64_t end = std::min(requestEnd, cEnd);
    if (begin >= end) continue;
    uint64_t done = begin;
    while (done < end) {
      const size_t outOff = static_cast<size_t>(done - offset);
      const uint64_t inChunk = done - c.logicalOffset;
      const size_t n = static_cast<size_t>(std::min<uint64_t>(end - done, 1ull << 20));
      if (c.type == kSparseRaw) {
        const uint64_t fileOffset = c.fileOffset + inChunk;
        size_t copied = 0;
        while (copied < n) {
          const ssize_t got = pread(fd_, out + outOff + copied, n - copied, static_cast<off_t>(fileOffset + copied));
          if (got <= 0) return Status::IoError;
          copied += static_cast<size_t>(got);
        }
      } else if (c.type == kSparseFill) {
        for (size_t j = 0; j < n; ++j) out[outOff + j] = c.fill[(inChunk + j) & 3u];
      }
      // DONT_CARE chunks intentionally retain the zero-fill initialized above.
      done += n;
    }
  }
  return Status::Ok;
}

std::string VirtioMmioBlock::diagnostics() const {
  std::ostringstream out;
  out << "label=" << label_
      << " sectors=" << capacitySectors()
      << " size_bytes=" << logicalSize_
      << " sparse_backing=" << (sparse_ ? "true" : "false")
      << " linear_view=" << (partitionView_ ? "true" : "false")
      << " synthetic_gpt_super=" << (superContainer_ ? "true" : "false")
      << " read_only=" << (readOnly_ ? "true" : "false")
      << " mmio_reads=" << mmioReads_.load(std::memory_order_relaxed)
      << " mmio_writes=" << mmioWrites_.load(std::memory_order_relaxed)
      << " read_requests=" << readRequests_.load(std::memory_order_relaxed)
      << " write_requests=" << writeRequests_.load(std::memory_order_relaxed)
      << " failed_requests=" << failedRequests_.load(std::memory_order_relaxed);
  return out.str();
}

Status VirtioMmioBlock::writeDisk(uint64_t offset, const uint8_t* in, uint64_t size) {
  if (readOnly_ || sparse_) return Status::DeviceError;
  if (!in || offset > logicalSize_ || size > logicalSize_ - offset) return Status::OutOfBounds;
  uint64_t done = 0;
  while (done < size) {
    const size_t chunk = static_cast<size_t>(std::min<uint64_t>(size - done, 1ull << 20));
    const ssize_t n = pwrite(fd_, in + done, chunk, static_cast<off_t>(offset + done));
    if (n <= 0) return Status::IoError;
    done += static_cast<uint64_t>(n);
  }
  return Status::Ok;
}

Status VirtioMmioBlock::processQueue() {
  if (!queueReady_ || queueNum_ == 0 || !queueDesc_ || !queueAvail_ || !queueUsed_ || !(status_ & kStatusDriverOk))
    return Status::DeviceError;
  uint64_t avIdxRaw = 0;
  Status st = bus_->read(GuestAddr(queueAvail_ + 2), 2, &avIdxRaw);
  if (st != Status::Ok) return st;
  const uint16_t availIdx = static_cast<uint16_t>(avIdxRaw);
  unsigned processed = 0;
  while (lastAvailIndex_ != availIdx && processed < queueNum_) {
    uint64_t headRaw = 0;
    const uint64_t availEnt = queueAvail_ + 4ull + (static_cast<uint64_t>(lastAvailIndex_ % queueNum_) * 2ull);
    st = bus_->read(GuestAddr(availEnt), 2, &headRaw);
    if (st != Status::Ok) return st;
    const uint16_t head = static_cast<uint16_t>(headRaw);
    uint32_t usedLen = 0;
    uint8_t requestStatus = kBlkIoErr;
    std::vector<uint8_t> data;
    struct Desc { uint64_t addr; uint32_t len, flags; uint16_t next; };
    std::vector<Desc> chain;
    uint16_t index = head;
    bool valid = true;
    for (unsigned hops = 0; hops < 32; ++hops) {
      if (index >= queueNum_) { valid = false; break; }
      uint8_t raw[16]{};
      st = readGuest(queueDesc_ + static_cast<uint64_t>(index) * 16ull, raw, sizeof(raw));
      if (st != Status::Ok) { valid = false; break; }
      Desc d{le64(raw), le32(raw + 8), le16(raw + 12), le16(raw + 14)};
      if (d.flags & kDescIndirect) { valid = false; break; }
      chain.push_back(d);
      if ((d.flags & kDescNext) == 0) break;
      index = d.next;
      if (hops == 31) valid = false;
    }
    if (valid && chain.size() >= 2 && chain[0].len >= 16 && (chain[0].flags & kDescWrite) == 0 &&
        (chain.back().flags & kDescWrite) != 0 && chain.back().len >= 1) {
      uint8_t hdr[16]{};
      if (readGuest(chain[0].addr, hdr, sizeof(hdr)) == Status::Ok) {
        const uint32_t type = le32(hdr);
        const uint64_t sector = le64(hdr + 8);
        requestStatus = kBlkUnsupported;
        if (type == kReqIn || type == kReqOut) {
          if (type == kReqIn) readRequests_.fetch_add(1, std::memory_order_relaxed);
          else writeRequests_.fetch_add(1, std::memory_order_relaxed);
          uint64_t total = 0;
          for (size_t i = 1; i + 1 < chain.size(); ++i) {
            if (chain[i].len > (64u << 20) || addOver(total, chain[i].len, &total)) { valid = false; break; }
          }
          if (valid && total && total <= (64ull << 20) && sector <= UINT64_MAX / 512) {
            uint64_t diskOff = sector * 512ull;
            if (type == kReqIn) {
              try { data.resize(static_cast<size_t>(total)); } catch (...) { valid = false; }
              if (valid && readDisk(diskOff, data.data(), total) == Status::Ok) {
                size_t copied = 0;
                for (size_t i = 1; i + 1 < chain.size(); ++i) {
                  if ((chain[i].flags & kDescWrite) == 0 || writeGuest(chain[i].addr, data.data() + copied, chain[i].len) != Status::Ok) { valid = false; break; }
                  copied += chain[i].len;
                }
                if (valid) { requestStatus = kBlkOk; usedLen = static_cast<uint32_t>(total + 1); }
              }
            } else if (!readOnly_ && (driverFeatures_[0] & (1u << 9))) {
              try { data.resize(static_cast<size_t>(total)); } catch (...) { valid = false; }
              if (valid) {
                size_t copied = 0;
                for (size_t i = 1; i + 1 < chain.size(); ++i) {
                  if (chain[i].flags & kDescWrite || readGuest(chain[i].addr, data.data() + copied, chain[i].len) != Status::Ok) { valid = false; break; }
                  copied += chain[i].len;
                }
                if (valid && writeDisk(diskOff, data.data(), total) == Status::Ok) { requestStatus = kBlkOk; usedLen = 1; }
              }
            } else requestStatus = kBlkIoErr;
          }
        } else if (type == kReqFlush) {
          requestStatus = readOnly_ ? kBlkOk : (fsync(fd_) == 0 ? kBlkOk : kBlkIoErr);
          usedLen = 1;
        } else if (type == kReqGetId) {
          if (chain.size() >= 3 && (chain[1].flags & kDescWrite) && chain[1].len >= 20 && writeGuest(chain[1].addr, "NCV-VIRTIO-BLOCK-0001", 20) == Status::Ok) {
            requestStatus = kBlkOk; usedLen = chain[1].len + 1;
          }
        }
      }
    }
    if (requestStatus != kBlkOk) failedRequests_.fetch_add(1, std::memory_order_relaxed);
    const Desc& statusDesc = chain.empty() ? Desc{} : chain.back();
    if (!chain.empty() && (statusDesc.flags & kDescWrite) && statusDesc.len >= 1) {
      if (writeGuest(statusDesc.addr, &requestStatus, 1) != Status::Ok) return Status::DeviceError;
    }
    uint8_t usedElement[8]{};
    putLe32(usedElement, head); putLe32(usedElement + 4, usedLen);
    const uint64_t usedEntry = queueUsed_ + 4ull + (static_cast<uint64_t>((lastAvailIndex_ % queueNum_)) * 8ull);
    if (writeGuest(usedEntry, usedElement, sizeof(usedElement)) != Status::Ok) return Status::DeviceError;
    ++lastAvailIndex_; ++processed;
  }
  uint8_t idxBytes[2]{}; putLe16(idxBytes, lastAvailIndex_);
  if (writeGuest(queueUsed_ + 2, idxBytes, sizeof(idxBytes)) != Status::Ok) return Status::DeviceError;
  if (processed) { interruptStatus_ |= 1u; updateIrq(); }
  return Status::Ok;
}

} // namespace ncv
