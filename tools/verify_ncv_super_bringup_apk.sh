#!/usr/bin/env bash
set -euo pipefail

APK="${1:-}"
EXPECTED='0.1.24-dev-llvm-path-autodetect-20261028'
if [[ -z "$APK" || ! -s "$APK" ]]; then
  echo "Usage: $0 /path/to/app-debug.apk" >&2
  exit 2
fi
for tool in unzip strings sha256sum python3; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing required verification tool: $tool" >&2; exit 3; }
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if ! unzip -p "$APK" lib/arm64-v8a/libncv.so > "$TMP/libncv.so" || [[ ! -s "$TMP/libncv.so" ]]; then
  echo "FAIL: APK has no arm64-v8a/libncv.so" >&2
  exit 4
fi
strings "$TMP/libncv.so" > "$TMP/native-strings.txt"
if ! grep -Fq "$EXPECTED" "$TMP/native-strings.txt"; then
  echo "FAIL: stale/different native engine. Expected marker: $EXPECTED" >&2
  grep -E '0\.1\.[0-9]+.*ncv|NCV_ENGINE_VERSION' "$TMP/native-strings.txt" | tail -10 || true
  exit 5
fi

KERNEL_ASSET='assets/ncv/guest/diagnostic/Image'
INITRD_ASSET='assets/ncv/guest/diagnostic/initramfs.cpio.gzip'
MANIFEST_ASSET='assets/ncv/guest/manifest.json'
unzip -l "$APK" > "$TMP/apk-list.txt"
for asset in "$KERNEL_ASSET" "$INITRD_ASSET" "$MANIFEST_ASSET"; do
  if ! grep -Fq "$asset" "$TMP/apk-list.txt"; then
    echo "FAIL: required guest asset missing from APK: $asset" >&2
    echo "APK diagnostic-related entries:" >&2
    grep -E 'assets/ncv/guest/(diagnostic|manifest)' "$TMP/apk-list.txt" >&2 || true
    echo "Generated assets and AGP merge output are checked by build_verify_ncv_super_bringup.sh." >&2
    echo "Build with ncv.linuxKernelSource + ncv.busyboxSource and ncv.requireDiagnosticGuestArtifacts=true." >&2
    exit 6
  fi
done

unzip -p "$APK" "$KERNEL_ASSET" > "$TMP/Image"
unzip -p "$APK" "$INITRD_ASSET" > "$TMP/initramfs.cpio.gz"
unzip -p "$APK" "$MANIFEST_ASSET" > "$TMP/manifest.json"
python3 - "$TMP" <<'PY'
import gzip, hashlib, json, pathlib, struct, sys
root = pathlib.Path(sys.argv[1])
manifest = json.loads((root / 'manifest.json').read_text())
for key, filename in [('diagnosticLinuxImage', 'Image'), ('diagnosticInitramfs', 'initramfs.cpio.gz')]:
    item = manifest.get(key)
    path = root / filename
    if not isinstance(item, dict):
        raise SystemExit(f'FAIL: manifest entry {key} is null/missing')
    data = path.read_bytes()
    if not data:
        raise SystemExit(f'FAIL: {filename} is empty')
    if item.get('sizeBytes') != len(data):
        raise SystemExit(f'FAIL: manifest size mismatch for {filename}')
    if item.get('sha256') != hashlib.sha256(data).hexdigest():
        raise SystemExit(f'FAIL: manifest SHA-256 mismatch for {filename}')
image = (root / 'Image').read_bytes()
if len(image) < 64 or image[0x38:0x3c] != b'ARM\x64':
    raise SystemExit('FAIL: diagnostic kernel is not a valid raw ARM64 Linux Image header (magic at 0x38)')
rd = (root / 'initramfs.cpio.gz').read_bytes()
try:
    unpacked = gzip.decompress(rd)
except Exception as exc:
    raise SystemExit(f'FAIL: initramfs gzip validation failed: {exc}')
if b'NCV-GUEST] INITRAMFS /init STARTED' not in unpacked:
    raise SystemExit('FAIL: initramfs does not contain the expected diagnostic /init marker')
if b'/init\x00' not in unpacked and b'./init\x00' not in unpacked:
    raise SystemExit('FAIL: initramfs does not contain an /init entry name')
print('PASS: manifest hashes/sizes match the packaged diagnostic kernel and initramfs')
print('PASS: kernel carries the ARM64 Linux Image magic')
print('PASS: gzip initramfs contains the NCV /init marker')
PY

echo "PASS: current NCV native engine marker found: $EXPECTED"
echo "PASS: diagnostic Linux Image and BusyBox initramfs are present in the APK"
echo "Next: use the primary 'Try Android userspace · NCV kernel + original ramdisk' action. The stock OEM-kernel path remains secondary."
