// NCV - motorun uygulama içi öz-testi (JNI nativeSelfTest bunu çağırır).
// Yalnızca küçük bir bare-metal payload'ı emüle edip UART çıktısını doğrular; Linux/Android boot'u KANITLAMAZ.
#pragma once
#include <string>

namespace ncv {
// ok: tüm kontroller geçtiyse true. Dönüş: insan okunur (ASCII) rapor.
std::string runSelfTest(bool* ok);
}  // namespace ncv
