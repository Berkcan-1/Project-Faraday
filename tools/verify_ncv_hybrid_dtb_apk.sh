#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 1 || ! -f "$1" ]]; then
  echo "Usage: $0 path/to/app-debug.apk" >&2
  exit 2
fi
APK=$1
EXPECTED='0.1.9-dev-pa-zero-pte-writer-20261027'
if ! command -v unzip >/dev/null 2>&1 || ! command -v strings >/dev/null 2>&1; then
  echo "Required tools: unzip and strings" >&2
  exit 2
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
if ! unzip -p "$APK" lib/arm64-v8a/libncv.so > "$TMP/libncv.so"; then
  echo "Could not extract lib/arm64-v8a/libncv.so from APK" >&2
  exit 1
fi
if [[ ! -s "$TMP/libncv.so" ]]; then
  echo "The APK has no non-empty arm64-v8a/libncv.so" >&2
  exit 1
fi
if strings "$TMP/libncv.so" | grep -Fq "$EXPECTED"; then
  echo "PASS: APK contains NCV engine $EXPECTED"
else
  echo "FAIL: expected engine ID $EXPECTED was not found in APK's libncv.so" >&2
  exit 1
fi
