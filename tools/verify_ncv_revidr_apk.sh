#!/usr/bin/env bash
set -euo pipefail
APK="${1:-app/build/outputs/apk/debug/app-debug.apk}"
EXPECTED="0.1.9-dev-pa-zero-pte-writer-20261027"
if [[ ! -f "$APK" ]]; then
  echo "APK not found: $APK" >&2
  exit 2
fi
if ! command -v unzip >/dev/null 2>&1 || ! command -v strings >/dev/null 2>&1; then
  echo "Required tools: unzip, strings (binutils)" >&2
  exit 2
fi
if ! unzip -l "$APK" | grep -q 'lib/arm64-v8a/libncv.so'; then
  echo "FAIL: APK does not contain lib/arm64-v8a/libncv.so" >&2
  exit 1
fi
if unzip -p "$APK" lib/arm64-v8a/libncv.so | strings | grep -Fq "$EXPECTED"; then
  echo "PASS: APK contains the expected NCV native engine build: $EXPECTED"
else
  echo "FAIL: APK contains an old/different libncv.so; expected marker: $EXPECTED" >&2
  echo "Run: ./gradlew --stop; rm -rf app/.cxx app/build; ./gradlew :app:assembleDebug --rerun-tasks" >&2
  exit 1
fi
