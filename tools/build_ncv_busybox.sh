#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 /path/to/busybox-source [output-directory]" >&2
  exit 2
fi

BUSYBOX_SRC="$(cd "$1" && pwd)"
OUT_DIR="${2:-$PWD/out-ncv-busybox}"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"

if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
  echo "Missing cross compiler: ${CROSS_COMPILE}gcc" >&2
  echo "On Ubuntu install gcc-aarch64-linux-gnu or set CROSS_COMPILE." >&2
  exit 3
fi
if [[ ! -f "$BUSYBOX_SRC/Makefile" ]]; then
  echo "Not a BusyBox source tree: $BUSYBOX_SRC" >&2
  exit 4
fi

if [[ ! -s "$OUT_DIR/.config" ]]; then
  make -C "$BUSYBOX_SRC" O="$OUT_DIR" CROSS_COMPILE="$CROSS_COMPILE" defconfig
fi

# Keep the build output separate from the source tree and force a static ARM64 binary.
if grep -q '^# CONFIG_STATIC is not set' "$OUT_DIR/.config"; then
  sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' "$OUT_DIR/.config"
elif grep -q '^CONFIG_STATIC=' "$OUT_DIR/.config"; then
  sed -i 's/^CONFIG_STATIC=.*/CONFIG_STATIC=y/' "$OUT_DIR/.config"
else
  printf '\nCONFIG_STATIC=y\n' >> "$OUT_DIR/.config"
fi

# BusyBox versions may add Kconfig options; empty answers select their defaults non-interactively.
set +o pipefail
yes '' | make -C "$BUSYBOX_SRC" O="$OUT_DIR" CROSS_COMPILE="$CROSS_COMPILE" oldconfig >/dev/null
make_status=${PIPESTATUS[1]}
set -o pipefail
if [[ "$make_status" -ne 0 ]]; then exit "$make_status"; fi
make -C "$BUSYBOX_SRC" O="$OUT_DIR" CROSS_COMPILE="$CROSS_COMPILE" -j"$JOBS" busybox

BIN="$OUT_DIR/busybox"
if [[ ! -s "$BIN" ]]; then
  echo "BusyBox build did not create $BIN" >&2
  exit 5
fi
if ! command -v readelf >/dev/null 2>&1; then
  echo "Missing readelf (install binutils)." >&2
  exit 6
fi
machine="$(readelf -h "$BIN" | sed -n 's/^[[:space:]]*\(Machine\|Makine\):[[:space:]]*//p' | head -n 1)"
if [[ "$machine" != *AArch64* ]]; then
  echo "Expected AArch64 BusyBox, got: ${machine:-unknown}" >&2
  exit 7
fi
if readelf -l "$BIN" | grep -q 'INTERP'; then
  echo "BusyBox is dynamically linked; CONFIG_STATIC did not take effect." >&2
  exit 8
fi
printf '\nNCV static AArch64 BusyBox built: %s (%s bytes)\n' "$BIN" "$(wc -c < "$BIN" | tr -d ' ')"
