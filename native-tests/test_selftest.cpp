#include <string>

#include "selftest/ncv_selftest.h"
#include "test_framework.h"

NT_TEST(selftest_passes_and_reports) {
  bool ok = false;
  const std::string rep = ncv::runSelfTest(&ok);
  CHECK(ok);
  CHECK(rep.find("PASS guest-hello-uart") != std::string::npos);
  CHECK(rep.find("PASS bus-bounds") != std::string::npos);
  CHECK(rep.find("FAIL") == std::string::npos);
}
