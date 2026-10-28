#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 /path/to/linux-kernel-source [output-directory]" >&2
  exit 2
fi

KERNEL_SRC="$(cd "$1" && pwd)"
OUT_DIR="${2:-$PWD/out-ncv-kernel}"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tools/ncv_llvm_toolchain.sh"
FRAGMENT="$ROOT/guest-board/ncv_linux_bringup.config"
COMPAT_FRAGMENT="$OUT_DIR/ncv_linux_bringup.compat.config"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"

if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
  echo "Missing cross compiler: ${CROSS_COMPILE}gcc" >&2
  echo "On Ubuntu install gcc-aarch64-linux-gnu and set CROSS_COMPILE if needed." >&2
  exit 3
fi
if [[ ! -x "$KERNEL_SRC/scripts/kconfig/merge_config.sh" ]]; then
  echo "Not a Linux kernel source tree (scripts/kconfig/merge_config.sh not found): $KERNEL_SRC" >&2
  exit 4
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

# Current mainline's Rust Binder depends on RUST_IS_AVAILABLE. That Kconfig
# predicate follows the kernel's LLVM/Rust toolchain checks; merely finding
# rustc (or seeing CONFIG_RUSTC_VERSION in .config) does not enable it.
LLVM_ARGS=()
USE_RUST_BINDER=0
if has_symbol ANDROID_BINDER_IPC_RUST "${ANDROID_KCONFIGS[@]}"; then
  USE_RUST_BINDER=1
  if ! ncv_configure_llvm_toolchain; then
    exit 6
  fi
  LLVM_ARGS=("LLVM=$NCV_LLVM_SETTING")
  echo "[NCV KCONFIG] Rust Binder is present; using LLVM=$NCV_LLVM_SETTING for defconfig, olddefconfig and Image build."
  echo "[NCV KCONFIG] Running the kernel's official Rust availability check before merging CONFIG_RUST..."
  if ! make -C "$KERNEL_SRC" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" "${LLVM_ARGS[@]}" rustavailable; then
    echo "ERROR: kernel Rust support is unavailable with LLVM=$NCV_LLVM_SETTING." >&2
    echo "Run this command from the same Linux source tree to see the full toolchain diagnosis:" >&2
    echo "  make -C '$KERNEL_SRC' ARCH=arm64 LLVM='$NCV_LLVM_SETTING' rustavailable" >&2
    echo "Install the exact rustc, rust-src, bindgen/libclang, and LLVM/Clang versions required by this kernel, then retry." >&2
    exit 6
  fi
elif ! has_symbol ANDROID_BINDER_IPC "${ANDROID_KCONFIGS[@]}"; then
  echo "ERROR: no supported Android Binder Kconfig symbol was found in drivers/android or drivers/staging/android." >&2
  exit 5
fi

"$ROOT/tools/prepare_ncv_kernel_config_fragment.sh" "$KERNEL_SRC" "$FRAGMENT" "$COMPAT_FRAGMENT"
make -C "$KERNEL_SRC" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" "${LLVM_ARGS[@]}" O="$OUT_DIR" defconfig
if [[ "$USE_RUST_BINDER" -eq 1 ]]; then
  # merge_config invokes make internally, so preserve the resolved LLVM path too.
  LLVM="$NCV_LLVM_SETTING" "$KERNEL_SRC/scripts/kconfig/merge_config.sh" -m -O "$OUT_DIR" "$OUT_DIR/.config" "$COMPAT_FRAGMENT"
else
  "$KERNEL_SRC/scripts/kconfig/merge_config.sh" -m -O "$OUT_DIR" "$OUT_DIR/.config" "$COMPAT_FRAGMENT"
fi
make -C "$KERNEL_SRC" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" "${LLVM_ARGS[@]}" O="$OUT_DIR" olddefconfig

required=(CONFIG_ARM64_4K_PAGES CONFIG_OF CONFIG_ARM_GIC CONFIG_ARM_ARCH_TIMER \
  CONFIG_SERIAL_AMBA_PL011 CONFIG_SERIAL_AMBA_PL011_CONSOLE CONFIG_SERIAL_EARLYCON CONFIG_VIRTIO \
  CONFIG_VIRTIO_MMIO CONFIG_VIRTIO_BLK CONFIG_EFI_PARTITION CONFIG_BLK_DEV_DM CONFIG_DM_VERITY \
  CONFIG_BLK_DEV_INITRD CONFIG_RD_GZIP CONFIG_DEVTMPFS CONFIG_PROC_FS CONFIG_SYSFS \
  CONFIG_EROFS_FS CONFIG_SECURITY CONFIG_AUDIT CONFIG_NET CONFIG_INET CONFIG_SECURITY_NETWORK \
  CONFIG_SECURITY_SELINUX CONFIG_CGROUPS CONFIG_NAMESPACES CONFIG_SECCOMP CONFIG_SECCOMP_FILTER)

if has_symbol DM_LINEAR "${MD_KCONFIGS[@]}"; then
  required+=(CONFIG_DM_LINEAR)
elif ! grep -q 'dm-linear\.o' "$KERNEL_SRC/drivers/md/Makefile"; then
  echo "ERROR: device-mapper linear target is unavailable (no CONFIG_DM_LINEAR symbol or dm-linear.o in dm-mod)." >&2
  exit 5
fi

if [[ "$USE_RUST_BINDER" -eq 1 ]]; then
  required+=(CONFIG_RUST CONFIG_ANDROID_BINDER_IPC_RUST)
else
  required+=(CONFIG_ANDROID_BINDER_IPC)
  if has_symbol ANDROID_BINDERFS "${ANDROID_KCONFIGS[@]}"; then required+=(CONFIG_ANDROID_BINDERFS); fi
fi

missing=0
for key in "${required[@]}"; do
  if ! grep -qx "$key=y" "$OUT_DIR/.config"; then
    echo "ERROR: required bring-up option did not resolve to y: $key" >&2
    missing=1
  fi
done
if [[ $missing -ne 0 ]]; then
  echo "Kconfig did not accept all required options. Relevant final config lines:" >&2
  grep -E '^(# )?CONFIG_(RUST|ANDROID_BINDER|DM_LINEAR|BLK_DEV_DM|DM_VERITY|AUDIT|NET=|INET=|SECURITY_NETWORK|SECURITY_SELINUX)' "$OUT_DIR/.config" >&2 || true
  if [[ "$USE_RUST_BINDER" -eq 1 ]] && ! grep -qx 'CONFIG_RUST=y' "$OUT_DIR/.config"; then
    echo "Rust availability check ran, but CONFIG_RUST is still disabled after LLVM=$NCV_LLVM_SETTING olddefconfig." >&2
    echo "Recheck: make -C '$KERNEL_SRC' ARCH=arm64 LLVM='$NCV_LLVM_SETTING' rustavailable" >&2
    echo "Also inspect RUST_IS_AVAILABLE/CC_IS_CLANG in the final .config and kernel Kconfig dependencies." >&2
  fi
  echo "Inspect $OUT_DIR/.config; no kernel Image was built." >&2
  exit 6
fi

make -C "$KERNEL_SRC" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" "${LLVM_ARGS[@]}" O="$OUT_DIR" -j"$JOBS" Image
IMAGE="$OUT_DIR/arch/arm64/boot/Image"
if [[ ! -s "$IMAGE" ]]; then
  echo "Build finished but Image was not produced at expected path: $IMAGE" >&2
  exit 7
fi
printf '\nNCV Linux Image built: %s\n' "$IMAGE"
printf 'Output directory: %s\n' "$OUT_DIR"
printf 'Reminder: compilation is not proof that NCV CPU/board emulation can boot this kernel. Test through NCV UART.\n'
