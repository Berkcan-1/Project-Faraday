#include "ncv_linux_image.h"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>
#include <zlib.h>

namespace ncv {
namespace {
constexpr uint64_t kArm64ImageMagic = 0x644d5241ull;
constexpr uint64_t kImageHeaderSize = 64;
constexpr uint64_t kImageAlignment = 2ull << 20;
constexpr uint64_t kMaxDecompressedImageBytes = 512ull << 20;

uint32_t readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t readLe64(const uint8_t* p) {
  return static_cast<uint64_t>(readLe32(p)) | (static_cast<uint64_t>(readLe32(p + 4)) << 32);
}
bool validRange(uint64_t base, uint64_t size) {
  return size != 0 && base <= std::numeric_limits<uint64_t>::max() - size;
}
}  // namespace

Status inspectArm64LinuxImage(const uint8_t* image, uint64_t len, uint64_t ramBase,
                              uint64_t ramSize, Arm64LinuxImageInfo* outInfo) {
  if (image == nullptr || outInfo == nullptr || len < kImageHeaderSize ||
      !validRange(ramBase, ramSize)) return Status::InvalidArgument;

  // ARM64 Image header: text_offset @ 0x08, image_size @ 0x10, flags @ 0x18,
  // magic @ 0x38. All fields are little-endian in current Image format.
  if (readLe32(image + 0x38) != kArm64ImageMagic) return Status::InvalidArgument;
  const uint64_t rawTextOffset = readLe64(image + 0x08);
  const uint64_t rawImageSize = readLe64(image + 0x10);
  const uint64_t flags = readLe64(image + 0x18);

  // NCV currently implements only little-endian AArch64 and 4 KiB guest page tables.
  if ((flags & 1u) != 0) return Status::InvalidArgument;  // big-endian kernel
  const uint64_t pageSizeCode = (flags >> 1) & 3u;
  if (pageSizeCode > 1u) return Status::InvalidArgument; // 16 KiB / 64 KiB kernel profile unsupported
  if ((flags & ~0xFull) != 0) return Status::InvalidArgument; // reserved flag bits must be zero

  // Legacy images with image_size=0 use text_offset=0x80000 by the Linux boot protocol.
  const uint64_t textOffset = rawImageSize == 0 ? 0x80000ull : rawTextOffset;
  if (rawImageSize != 0 && rawImageSize < len) return Status::InvalidArgument;
  const uint64_t effectiveImageSize = rawImageSize == 0 ? len : rawImageSize;

  const uint64_t ramEnd = ramBase + ramSize;
  if (ramBase > std::numeric_limits<uint64_t>::max() - (kImageAlignment - 1))
    return Status::InvalidArgument;
  const uint64_t alignedBase = (ramBase + kImageAlignment - 1) & ~(kImageAlignment - 1);
  if (alignedBase > std::numeric_limits<uint64_t>::max() - textOffset)
    return Status::InvalidArgument;
  const uint64_t loadAddress = alignedBase + textOffset;
  if (loadAddress < ramBase || loadAddress > ramEnd ||
      effectiveImageSize > ramEnd - loadAddress || len > ramEnd - loadAddress)
    return Status::OutOfBounds;

  Arm64LinuxImageInfo info;
  info.fileSize = len;
  info.textOffset = textOffset;
  info.imageSize = effectiveImageSize;
  info.flags = flags;
  info.loadAddress = loadAddress;
  info.headerImageSizeKnown = rawImageSize != 0;
  *outInfo = info;
  return Status::Ok;
}

Status loadArm64LinuxImage(Bus& bus, const uint8_t* image, uint64_t len, uint64_t ramBase,
                           uint64_t ramSize, Arm64LinuxImageInfo* outInfo) {
  if (image == nullptr || len == 0) return Status::InvalidArgument;

  // A gzip-wrapped Image starts with the standard RFC 1952 magic. Keep decompression bounded;
  // zlib's gzip mode also validates the stream checksum/footer before returning Z_STREAM_END.
  std::vector<uint8_t> decompressed;
  const uint8_t* payload = image;
  uint64_t payloadLen = len;
  if (len >= 2 && image[0] == 0x1Fu && image[1] == 0x8Bu) {
    if (len > static_cast<uint64_t>(std::numeric_limits<uInt>::max())) return Status::OutOfBounds;
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(image));
    stream.avail_in = static_cast<uInt>(len);
    if (inflateInit2(&stream, MAX_WBITS + 16) != Z_OK) return Status::OutOfMemory;

    std::array<uint8_t, 64 * 1024> chunk{};
    int ret = Z_OK;
    Status decompressionStatus = Status::Ok;
    try {
      while (ret == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
        stream.avail_out = static_cast<uInt>(chunk.size());
        ret = inflate(&stream, Z_NO_FLUSH);
        const size_t produced = chunk.size() - stream.avail_out;
        if (produced > kMaxDecompressedImageBytes - decompressed.size()) {
          decompressionStatus = Status::OutOfBounds;
          break;
        }
        decompressed.insert(decompressed.end(), chunk.begin(), chunk.begin() + produced);
      }
    } catch (const std::bad_alloc&) {
      decompressionStatus = Status::OutOfMemory;
    } catch (...) {
      decompressionStatus = Status::OutOfMemory;
    }
    const bool complete = ret == Z_STREAM_END && stream.avail_in == 0;
    inflateEnd(&stream);
    if (decompressionStatus != Status::Ok) return decompressionStatus;
    if (!complete || decompressed.empty()) return Status::InvalidArgument;
    payload = decompressed.data();
    payloadLen = decompressed.size();
  }

  Arm64LinuxImageInfo info;
  const Status inspected = inspectArm64LinuxImage(payload, payloadLen, ramBase, ramSize, &info);
  if (inspected != Status::Ok) return inspected;
  const Status loaded = bus.loadBytes(GuestAddr(info.loadAddress), payload, payloadLen);
  if (loaded != Status::Ok) return loaded;
  if (outInfo != nullptr) *outInfo = info;
  return Status::Ok;
}

}  // namespace ncv
