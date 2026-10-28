// NCV - ham ikili payload yükleyici (Aşama 5 için). Kernel/initramfs/DTB biçimleri henüz YOK.
#pragma once
#include <cstdint>

#include "../core/ncv_types.h"
#include "../memory/ncv_bus.h"

namespace ncv {

// Dosyayı (boyut sınırı kontrollü) guest RAM'ine yükler. Güvenilmeyen girdi varsayılır.
Status loadRawFile(Bus& bus, GuestAddr addr, const char* path, uint64_t maxBytes, uint64_t* loadedBytes);

}  // namespace ncv
