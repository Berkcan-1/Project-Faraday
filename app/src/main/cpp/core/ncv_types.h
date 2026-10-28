// NCV - ortak tipler. Bu dosya Android'e bağımlı DEĞİLDİR (host'ta test edilebilir).
#pragma once
#include <cstdint>

namespace ncv {

// Guest fiziksel adresi. Host pointer'ı (veya düz uint64_t) ile karışmasın diye ayrı tip.
struct GuestAddr {
  uint64_t value;
  constexpr GuestAddr() : value(0) {}
  explicit constexpr GuestAddr(uint64_t v) : value(v) {}
};

// Hata kodları. Exception sınırlar arası (JNI) taşınmaz; durumlar bu enum ile taşınır.
enum class Status : int32_t {
  Ok = 0,
  InvalidArgument,  // geçersiz parametre / erişim boyutu / adres taşması
  InvalidState,     // bu durumda izin verilmeyen işlem
  Unmapped,         // adreste bölge yok
  OutOfBounds,      // erişim bölge sınırını aşıyor
  Overlap,          // bölge çakışması
  OutOfMemory,      // host bellek limiti / ayırma hatası
  DeviceError,      // aygıt erişimi reddetti (desteklenmeyen register vb.)
  IoError,          // dosya hatası
};

inline const char* statusName(Status s) {
  switch (s) {
    case Status::Ok: return "Ok";
    case Status::InvalidArgument: return "InvalidArgument";
    case Status::InvalidState: return "InvalidState";
    case Status::Unmapped: return "Unmapped";
    case Status::OutOfBounds: return "OutOfBounds";
    case Status::Overlap: return "Overlap";
    case Status::OutOfMemory: return "OutOfMemory";
    case Status::DeviceError: return "DeviceError";
    case Status::IoError: return "IoError";
  }
  return "Unknown";
}

}  // namespace ncv
