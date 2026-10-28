#include "ncv_loader.h"

#include <cstdio>
#include <vector>

namespace ncv {

Status loadRawFile(Bus& bus, GuestAddr addr, const char* path, uint64_t maxBytes, uint64_t* loadedBytes) {
  if (path == nullptr) return Status::InvalidArgument;
  if (loadedBytes) *loadedBytes = 0;
  FILE* f = std::fopen(path, "rb");
  if (f == nullptr) return Status::IoError;
  Status result = Status::Ok;
  std::vector<uint8_t> buf;
  uint8_t chunk[4096];
  for (;;) {
    const size_t n = std::fread(chunk, 1, sizeof(chunk), f);
    if (n == 0) break;
    if (buf.size() + n > maxBytes) { result = Status::InvalidArgument; break; }  // sınırı aşan dosya
    try {
      buf.insert(buf.end(), chunk, chunk + n);
    } catch (...) {
      result = Status::OutOfMemory;
      break;
    }
  }
  if (result == Status::Ok && std::ferror(f)) result = Status::IoError;
  std::fclose(f);
  if (result != Status::Ok) return result;
  result = bus.loadBytes(addr, buf.data(), buf.size());
  if (result == Status::Ok && loadedBytes) *loadedBytes = buf.size();
  return result;
}

}  // namespace ncv
