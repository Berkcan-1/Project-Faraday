#!/usr/bin/env bash
set -euo pipefail
APK="${1:-app/build/outputs/apk/debug/app-debug.apk}"
EXPECTED="0.1.9-dev-pa-zero-pte-writer-20261027"
if [[ ! -f "$APK" ]]; then echo "APK not found: $APK" >&2; exit 2; fi
command -v unzip >/dev/null && command -v strings >/dev/null || { echo "Required tools: unzip, strings" >&2; exit 2; }
TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT
if ! unzip -p "$APK" lib/arm64-v8a/libncv.so > "$TMP"; then
  echo "FAIL: APK does not contain lib/arm64-v8a/libncv.so" >&2; exit 1
fi
if [[ ! -s "$TMP" ]]; then echo "FAIL: empty libncv.so in APK" >&2; exit 1; fi
if strings "$TMP" | grep -Fq "$EXPECTED"; then
  echo "PASS: APK contains NCV engine $EXPECTED"
else
  echo "FAIL: expected engine marker $EXPECTED not found. Rebuild with: ./gradlew --stop; rm -rf app/.cxx app/build; ./gradlew :app:assembleDebug --rerun-tasks" >&2
  exit 1
fi
