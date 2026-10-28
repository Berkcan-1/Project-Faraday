#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 /path/to/static-aarch64-busybox [output.cpio.gz]" >&2
  exit 2
fi
BUSYBOX="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
OUT="${2:-$PWD/ncv-test-initramfs.cpio.gz}"
mkdir -p "$(dirname "$OUT")"
OUT="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"

if [[ ! -s "$BUSYBOX" ]]; then
  echo "BusyBox binary not found or empty: $BUSYBOX" >&2
  exit 3
fi
if ! command -v readelf >/dev/null 2>&1; then
  echo "Required host tool: readelf (Ubuntu package: binutils)" >&2
  exit 4
fi
machine="$(readelf -h "$BUSYBOX" 2>/dev/null | sed -n 's/^[[:space:]]*\(Machine\|Makine\):[[:space:]]*//p' | head -n 1)"
if [[ "$machine" != *AArch64* ]]; then
  echo "BusyBox must be built for AArch64, but ELF machine is: ${machine:-unknown}" >&2
  exit 5
fi
if readelf -l "$BUSYBOX" 2>/dev/null | grep -q 'INTERP'; then
  echo "BusyBox is dynamically linked. Rebuild it with Settings -> Build static binary (no shared libs)." >&2
  exit 6
fi
if ! command -v cpio >/dev/null 2>&1 || ! command -v gzip >/dev/null 2>&1; then
  echo "Required host tools: cpio and gzip (Ubuntu: sudo apt install cpio gzip)" >&2
  exit 7
fi

ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT
mkdir -p "$ROOT/bin" "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/tmp"
install -m 0755 "$BUSYBOX" "$ROOT/bin/busybox"
ln -s busybox "$ROOT/bin/sh"
ln -s busybox "$ROOT/bin/mount"
ln -s busybox "$ROOT/bin/sleep"
ln -s busybox "$ROOT/bin/dmesg"
ln -s busybox "$ROOT/bin/uname"
cat > "$ROOT/init" <<'INIT'
#!/bin/sh
PATH=/bin
export PATH
# Emit a UART marker before doing any optional mounts so early /init execution is observable.
echo "[NCV-GUEST] INITRAMFS /init STARTED"
/bin/busybox uname -a
/bin/busybox mount -t devtmpfs devtmpfs /dev 2>/dev/null && echo "[NCV-GUEST] MOUNT_OK devtmpfs" || echo "[NCV-GUEST] MOUNT_WARN devtmpfs"
/bin/busybox mount -t proc proc /proc 2>/dev/null && echo "[NCV-GUEST] MOUNT_OK proc" || echo "[NCV-GUEST] MOUNT_WARN proc"
/bin/busybox mount -t sysfs sysfs /sys 2>/dev/null && echo "[NCV-GUEST] MOUNT_OK sysfs" || echo "[NCV-GUEST] MOUNT_WARN sysfs"
/bin/busybox mount -t tmpfs tmpfs /tmp 2>/dev/null && echo "[NCV-GUEST] MOUNT_OK tmpfs" || echo "[NCV-GUEST] MOUNT_WARN tmpfs"
echo "[NCV-GUEST] cmdline:"
/bin/busybox cat /proc/cmdline 2>/dev/null || true
echo "[NCV-GUEST] block inventory from /proc/partitions:"
/bin/busybox cat /proc/partitions 2>/dev/null || true
echo "[NCV-GUEST] block nodes:"
/bin/busybox ls -l /dev/vda /dev/vda1 /dev/vdb /dev/vdc /dev/vdd /dev/vde /dev/vdf 2>/dev/null || true
# Read the carrier GPT and host-side logical extent views through the actual guest
# block driver. This produces a measurable result; it does not launch Android init.
for dev in /dev/vda /dev/vda1 /dev/vdb /dev/vdc /dev/vdd /dev/vde /dev/vdf; do
    if [ -b "$dev" ]; then
        if /bin/busybox dd if="$dev" of=/dev/null bs=512 count=1 2>/dev/null; then
            echo "[NCV-GUEST] BLOCK_READ_OK $dev"
        else
            echo "[NCV-GUEST] BLOCK_READ_FAILED $dev"
        fi
    fi
done
echo "[NCV-GUEST] recent kernel messages:"
/bin/busybox dmesg | /bin/busybox tail -n 35 2>/dev/null || true
echo "[NCV-GUEST] This is a real NCV Linux/storage bring-up, not Android userspace or boot animation."
while true; do /bin/busybox sleep 5; done
INIT
chmod 0755 "$ROOT/init"
(
  cd "$ROOT"
  find . -print0 | cpio --null -o --format=newc --owner=0:0 2>/dev/null | gzip -9 > "$OUT"
)
if [[ ! -s "$OUT" ]]; then
  echo "Failed to create initramfs: $OUT" >&2
  exit 8
fi
printf 'Created test initramfs: %s (%s bytes)\n' "$OUT" "$(wc -c < "$OUT" | tr -d ' ')"
printf 'This test image includes a static BusyBox and /init; it is not Android ramdisk.\n'
