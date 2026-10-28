#!/usr/bin/env bash
# Shared LLVM/Rust toolchain resolver for NCV's optional guest Linux kernel build.
# This is sourced by build_ncv_kernel.sh so it can export PATH/LIBCLANG_PATH
# inside Gradle's Exec child process, where interactive-shell exports may be absent.

ncv_configure_llvm_toolchain() {
  local candidate root="" selected=""
  local -a candidates=()

  if [[ -n "${NCV_LLVM_TOOLCHAIN_DIR:-}" ]]; then
    candidates+=("$NCV_LLVM_TOOLCHAIN_DIR")
  fi
  if [[ -n "${LLVM_PREFIX:-}" ]]; then
    candidates+=("$LLVM_PREFIX")
  fi
  if [[ -n "${LLVM:-}" && "$LLVM" != "1" && "$LLVM" != "0" ]]; then
    candidates+=("$LLVM")
  fi
  # Auto-discover the toolchain installed by the NCV setup steps, even when
  # Gradle was launched without inheriting the user's interactive PATH.
  candidates+=("$HOME"/toolchains/ncv/*)
  candidates+=("$HOME"/.local/opt/llvm-*)
  candidates+=(/opt/llvm /opt/llvm-*)

  # Prefer a complete bundled LLVM + bindgen + libclang directory.
  for candidate in "${candidates[@]}"; do
    [[ -d "$candidate" ]] || continue
    if [[ -x "$candidate/bin/clang" ]]; then
      root="$candidate"
    elif [[ "$(basename "$candidate")" == "bin" && -x "$candidate/clang" ]]; then
      root="$(dirname "$candidate")"
    else
      continue
    fi
    if [[ -x "$root/bin/bindgen" ]] && compgen -G "$root/lib/libclang.so*" >/dev/null; then
      selected="$root"
      break
    fi
  done

  if [[ -n "$selected" ]]; then
    NCV_LLVM_ROOT="$selected"
    NCV_LLVM_BIN_DIR="$selected/bin"
    # Kbuild supports an explicit LLVM tool directory ending in slash. This
    # avoids relying on Gradle's inherited PATH to find a command named clang.
    NCV_LLVM_SETTING="$NCV_LLVM_BIN_DIR/"
    export PATH="$NCV_LLVM_BIN_DIR:$HOME/.cargo/bin:$PATH"
    export LIBCLANG_PATH="$selected/lib"
    export LLVM="$NCV_LLVM_SETTING"
    echo "[NCV TOOLCHAIN] Discovered LLVM/Rust tools at $selected"
    echo "[NCV TOOLCHAIN] Using LLVM=$NCV_LLVM_SETTING"
    echo "[NCV TOOLCHAIN] clang=$(command -v clang)"
    echo "[NCV TOOLCHAIN] bindgen=$(command -v bindgen)"
    return 0
  fi

  # System-installed LLVM remains supported when it is already on PATH.
  if command -v clang >/dev/null 2>&1 && command -v bindgen >/dev/null 2>&1; then
    NCV_LLVM_ROOT=""
    NCV_LLVM_BIN_DIR="$(dirname "$(command -v clang)")"
    NCV_LLVM_SETTING="1"
    export PATH="$HOME/.cargo/bin:$PATH"
    export LLVM="$NCV_LLVM_SETTING"
    echo "[NCV TOOLCHAIN] Using system LLVM tools from PATH (LLVM=1)."
    echo "[NCV TOOLCHAIN] clang=$(command -v clang)"
    echo "[NCV TOOLCHAIN] bindgen=$(command -v bindgen)"
    return 0
  fi

  echo "ERROR: Rust Binder requires clang, bindgen, and libclang." >&2
  echo "Searched NCV_LLVM_TOOLCHAIN_DIR, LLVM_PREFIX, ~/toolchains/ncv/*, ~/.local/opt/llvm-*, /opt/llvm*, and PATH." >&2
  echo "Set NCV_LLVM_TOOLCHAIN_DIR to the LLVM package root (the directory containing bin/ and lib/)." >&2
  echo "Current PATH: $PATH" >&2
  echo "clang: $(command -v clang 2>/dev/null || echo missing)" >&2
  echo "bindgen: $(command -v bindgen 2>/dev/null || echo missing)" >&2
  return 1
}
