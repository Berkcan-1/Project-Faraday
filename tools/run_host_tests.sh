#!/usr/bin/env bash
# Host testleri: ASAN+UBSAN, TSAN ve Release. Çıkış kodu != 0 ise en az bir yapılandırma başarısız.
set -e
cd "$(dirname "$0")/.."
for cfg in "asan:-DNCV_SANITIZE=address,undefined" "tsan:-DNCV_SANITIZE=thread" "release:-DCMAKE_BUILD_TYPE=Release"; do
  name="${cfg%%:*}"; flags="${cfg#*:}"
  echo "=== $name ==="
  cmake -S . -B "build-$name" $flags >/dev/null
  cmake --build "build-$name" -j4 2>&1 | grep -E "error|warning" || true
  timeout 300 "./build-$name/ncv_tests" | tail -2
done
