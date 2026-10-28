// Bağımlılıksız mini test çatısı (ağ erişimi olmadan derlenebilsin diye).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/ncv_types.h"

namespace nt {
struct Case { const char* name; void (*fn)(); };
inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
inline int& failures() { static int f = 0; return f; }
struct Reg { Reg(const char* n, void (*f)()) { registry().push_back({n, f}); } };
inline std::string hex(uint64_t v) { char b[32]; std::snprintf(b, sizeof b, "%llx", (unsigned long long)v); return b; }
inline void fail(const char* file, int line, const std::string& msg) {
  std::printf("    FAIL %s:%d: %s\n", file, line, msg.c_str());
  failures()++;
}
inline const char* nm(ncv::Status s) { return ncv::statusName(s); }
}  // namespace nt

#define NT_TEST(name) static void name(); static nt::Reg reg_##name(#name, name); static void name()
#define CHECK(c) do { if (!(c)) nt::fail(__FILE__, __LINE__, #c); } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); \
  if (!(_a == _b)) nt::fail(__FILE__, __LINE__, std::string(#a " == " #b "  (got 0x") + nt::hex(static_cast<uint64_t>(_a)) + ", want 0x" + nt::hex(static_cast<uint64_t>(_b)) + ")"); } while (0)
#define CHECK_ST(a, b) do { ncv::Status _a = (a); ncv::Status _b = (b); \
  if (_a != _b) nt::fail(__FILE__, __LINE__, std::string(#a "  (got ") + nt::nm(_a) + ", want " + nt::nm(_b) + ")"); } while (0)
