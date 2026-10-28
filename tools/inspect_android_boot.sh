#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  tools/inspect_android_boot.sh /path/to/boot.img /path/to/aosp-root [output-dir]

Uses AOSP's unpack_bootimg.py to inspect the boot container and reports whether
it contains a kernel and a non-empty ramdisk. It does not modify the input image.
USAGE
}

if [[ $# -lt 2 || $# -gt 3 ]]; then usage; exit 2; fi
boot_img=$(realpath "$1")
aosp_root=$(realpath "$2")
out_dir=${3:-"$(dirname "$boot_img")/ncv-unpacked-boot-$(date +%Y%m%d-%H%M%S)"}

if [[ ! -f "$boot_img" ]]; then echo "ERROR: boot image not found: $boot_img" >&2; exit 2; fi
unpack_tool="$aosp_root/system/tools/mkbootimg/unpack_bootimg.py"
if [[ ! -f "$unpack_tool" ]]; then
  echo "ERROR: AOSP unpack_bootimg.py not found at: $unpack_tool" >&2
  echo "Pass the root of your synced AOSP tree as the second argument." >&2
  exit 2
fi

mkdir -p "$out_dir"
out_dir=$(realpath "$out_dir")
echo "[NCV] Input image: $boot_img"
echo "[NCV] Unpack output: $out_dir"
echo "[NCV] AOSP tool: $unpack_tool"
python3 "$unpack_tool" --boot_img "$boot_img" --out "$out_dir"

echo
echo "[NCV] Extracted top-level files:"
find "$out_dir" -maxdepth 1 -type f -printf '%f\t%s bytes\n' | sort

kernel_file="$out_dir/kernel"
ramdisk_file="$out_dir/ramdisk"
if [[ -f "$kernel_file" && -s "$kernel_file" ]]; then
  echo "[NCV] KERNEL: present ($(stat -c '%s' "$kernel_file") bytes)"
else
  echo "[NCV] KERNEL: missing or empty"
fi
if [[ -f "$ramdisk_file" && -s "$ramdisk_file" ]]; then
  echo "[NCV] RAMDISK: present ($(stat -c '%s' "$ramdisk_file") bytes)"
  echo "[NCV] A boot.img-only parsing attempt may pass this requirement, but board/vendor compatibility is still required."
else
  echo "[NCV] RAMDISK: missing/empty. This boot.img alone cannot provide a generic initramfs to NCV."
  echo "[NCV] Select the matching init_boot.img, or build a monolithic NCV-specific boot image with a compatible ramdisk."
fi

echo "[NCV] Reminder: vendor_boot can carry the vendor ramdisk and DTB. A missing vendor_boot may still prevent a real guest kernel from booting."
