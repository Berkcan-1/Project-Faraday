#include "test_framework.h"

int main() {
  int failedCases = 0;
  for (const nt::Case& c : nt::registry()) {
    const int before = nt::failures();
    c.fn();
    const bool ok = nt::failures() == before;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", c.name);
    if (!ok) failedCases++;
  }
  std::printf("\n%zu test, %d basarisiz, %d kontrol hatasi\n", nt::registry().size(), failedCases, nt::failures());
  return failedCases == 0 ? 0 : 1;
}
