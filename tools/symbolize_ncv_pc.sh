#!/usr/bin/env bash
set -euo pipefail
if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "Usage: $0 matching-vmlinux PC [path/to/llvm-addr2line]" >&2
  exit 2
fi
VMLINUX=$1
PC=$2
TOOL=${3:-}
if [[ ! -f "$VMLINUX" ]]; then
  echo "vmlinux not found: $VMLINUX" >&2
  exit 2
fi
if [[ -z "$TOOL" ]]; then
  if command -v llvm-addr2line >/dev/null 2>&1; then TOOL=$(command -v llvm-addr2line)
  elif command -v aarch64-linux-gnu-addr2line >/dev/null 2>&1; then TOOL=$(command -v aarch64-linux-gnu-addr2line)
  elif [[ -n "${ANDROID_NDK_HOME:-}" && -x "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-addr2line" ]]; then TOOL="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-addr2line"
  else
    echo "No llvm-addr2line or aarch64-linux-gnu-addr2line found. Pass the tool as the third argument." >&2
    exit 2
  fi
fi
"$TOOL" -f -C -e "$VMLINUX" "$PC"
