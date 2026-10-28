#!/usr/bin/env bash
# Generate a kernel-version-aware NCV config fragment.
# Handles modern mainline where dm-linear is built into dm-mod and Binder is Rust-based.
set -euo pipefail
if [[ $# -ne 3 ]]; then
  echo "Usage: $0 /path/to/linux-kernel-source base-fragment output-fragment" >&2
  exit 2
fi
KERNEL_SRC="$(cd "$1" && pwd)"
BASE_FRAGMENT="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
OUTPUT_FRAGMENT="$3"
if [[ ! -f "$BASE_FRAGMENT" ]]; then echo "ERROR: base fragment not found: $BASE_FRAGMENT" >&2; exit 2; fi
if [[ ! -f "$KERNEL_SRC/drivers/md/Kconfig" || ! -f "$KERNEL_SRC/drivers/md/Makefile" ]]; then
  echo "ERROR: expected drivers/md/Kconfig and drivers/md/Makefile in $KERNEL_SRC" >&2
  exit 3
fi

has_symbol() {
  local symbol="$1"; shift
  local file
  for file in "$@"; do
    [[ -f "$file" ]] || continue
    if grep -Eq "^[[:space:]]*config[[:space:]]+${symbol}([[:space:]]|$)" "$file"; then return 0; fi
  done
  return 1
}
ANDROID_KCONFIGS=("$KERNEL_SRC/drivers/android/Kconfig" "$KERNEL_SRC/drivers/staging/android/Kconfig")
MD_KCONFIGS=("$KERNEL_SRC/drivers/md/Kconfig")
mkdir -p "$(dirname "$OUTPUT_FRAGMENT")"
cp "$BASE_FRAGMENT" "$OUTPUT_FRAGMENT"
printf '\n# Generated compatibility options for this kernel source tree.\n' >> "$OUTPUT_FRAGMENT"

# dm-linear was historically an independent tristate. Newer mainline links dm-linear.o
# directly into dm-mod-y and intentionally has no CONFIG_DM_LINEAR symbol.
if has_symbol DM_LINEAR "${MD_KCONFIGS[@]}"; then
  printf 'CONFIG_DM_LINEAR=y\n' >> "$OUTPUT_FRAGMENT"
  echo "[NCV KCONFIG] CONFIG_DM_LINEAR exists; enabling it built-in."
elif grep -q 'dm-linear\.o' "$KERNEL_SRC/drivers/md/Makefile"; then
  echo "[NCV KCONFIG] CONFIG_DM_LINEAR is absent; dm-linear.o is built into dm-mod by this kernel tree."
else
  echo "ERROR: this kernel tree has neither CONFIG_DM_LINEAR nor dm-linear.o linked into dm-mod." >&2
  exit 4
fi

# Linux mainline has transitioned from the legacy C Binder Kconfig to Rust Binder.
if has_symbol ANDROID_BINDER_IPC_RUST "${ANDROID_KCONFIGS[@]}"; then
  printf 'CONFIG_RUST=y\nCONFIG_ANDROID_BINDER_IPC_RUST=y\n' >> "$OUTPUT_FRAGMENT"
  echo "[NCV KCONFIG] Using Rust Binder (CONFIG_ANDROID_BINDER_IPC_RUST); kernel Rust toolchain must be available."
elif has_symbol ANDROID_BINDER_IPC "${ANDROID_KCONFIGS[@]}"; then
  printf 'CONFIG_ANDROID_BINDER_IPC=y\n' >> "$OUTPUT_FRAGMENT"
  if has_symbol ANDROID_BINDERFS "${ANDROID_KCONFIGS[@]}"; then
    printf 'CONFIG_ANDROID_BINDERFS=y\n' >> "$OUTPUT_FRAGMENT"
    echo "[NCV KCONFIG] Using legacy Binder IPC + BinderFS."
  else
    echo "[NCV KCONFIG] Using legacy Binder IPC; BinderFS is not a separate Kconfig symbol in this kernel."
  fi
else
  echo "ERROR: no supported Android Binder Kconfig symbol was found in drivers/android or drivers/staging/android." >&2
  echo "This source tree cannot provide the Binder ABI expected by Android userspace through the supported profiles." >&2
  exit 5
fi

if has_symbol ANDROID_BINDER_DEVICES "${ANDROID_KCONFIGS[@]}"; then
  printf 'CONFIG_ANDROID_BINDER_DEVICES="binder,hwbinder,vndbinder"\n' >> "$OUTPUT_FRAGMENT"
fi
