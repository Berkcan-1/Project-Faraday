#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
INSTALL=0
if [[ "${1:-}" == "--install" ]]; then INSTALL=1; shift; fi
if [[ $# -gt 1 ]]; then
  echo "Usage: $0 [--install] [path/to/app-debug.apk]" >&2
  exit 2
fi
APK="${1:-$ROOT/app/build/outputs/apk/debug/app-debug.apk}"

if [[ ! -s local.properties ]]; then
  echo "ERROR: local.properties is missing. Configure Android SDK and NCV Linux/BusyBox source paths first." >&2
  exit 3
fi

# Repair the malformed setting emitted by the previous prototype archive, where the
# required property had accidentally been appended to sdk.dir on the same line.
if grep -q '^sdk\.dir=.*ncv\.requireDiagnosticGuestArtifacts=true$' local.properties; then
  sed -i '/^sdk\.dir=/s/ncv\.requireDiagnosticGuestArtifacts=true$//' local.properties
fi
if ! grep -q '^ncv.requireDiagnosticGuestArtifacts=true$' local.properties; then
  printf '\n%s\n' 'ncv.requireDiagnosticGuestArtifacts=true' >> local.properties
fi
if ! grep -q '^ncv.packageGuestArtifacts=true$' local.properties; then
  if grep -q '^ncv.packageGuestArtifacts=' local.properties; then
    echo "ERROR: ncv.packageGuestArtifacts is explicitly not true in local.properties." >&2
    exit 4
  fi
  printf '%s\n' 'ncv.packageGuestArtifacts=true' >> local.properties
fi

get_prop() { sed -n "s/^$1=//p" local.properties | tail -n 1; }
KERNEL_SRC="$(get_prop ncv.linuxKernelSource)"
BUSYBOX_SRC="$(get_prop ncv.busyboxSource)"
CROSS_COMPILE="$(get_prop ncv.crossCompile)"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"

if [[ -z "$KERNEL_SRC" || ! -f "$KERNEL_SRC/Makefile" || ! -x "$KERNEL_SRC/scripts/kconfig/merge_config.sh" ]]; then
  echo "ERROR: ncv.linuxKernelSource is not a usable Linux kernel source tree: ${KERNEL_SRC:-<empty>}" >&2
  echo "Expected Makefile and executable scripts/kconfig/merge_config.sh." >&2
  exit 5
fi
if [[ -z "$BUSYBOX_SRC" || ! -f "$BUSYBOX_SRC/Makefile" ]]; then
  echo "ERROR: ncv.busyboxSource is not a usable BusyBox source tree: ${BUSYBOX_SRC:-<empty>}" >&2
  exit 6
fi
for cmd in make cpio gzip readelf python3 unzip sha256sum; do
  command -v "$cmd" >/dev/null 2>&1 || { echo "ERROR: required host tool not found: $cmd" >&2; exit 7; }
done
command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1 || {
  echo "ERROR: cross compiler not found: ${CROSS_COMPILE}gcc" >&2
  echo "Ubuntu package: gcc-aarch64-linux-gnu" >&2
  exit 8
}

if [[ -s gradle/wrapper/gradle-wrapper.jar && -x ./gradlew ]]; then
  GRADLE=(./gradlew --no-daemon)
elif command -v gradle >/dev/null 2>&1; then
  GRADLE=(gradle --no-daemon)
else
  cached=""
  for candidate in "$HOME"/.gradle/wrapper/dists/gradle-8.13-bin/*/gradle-8.13/bin/gradle; do
    if [[ -x "$candidate" ]]; then cached="$candidate"; break; fi
  done
  if [[ -n "$cached" ]]; then GRADLE=("$cached" --no-daemon)
  else
    echo "ERROR: neither Gradle Wrapper JAR, a system gradle executable, nor a cached Gradle 8.13 distribution is available." >&2
    echo "Open this project in Android Studio and use the Build menu, or restore gradle/wrapper/gradle-wrapper.jar." >&2
    exit 9
  fi
fi

echo "[NCV BUILD] Linux source: $KERNEL_SRC"
echo "[NCV BUILD] BusyBox source: $BUSYBOX_SRC"
echo "[NCV BUILD] Cross compiler: ${CROSS_COMPILE}gcc"
echo "[NCV BUILD] Checking kernel Kconfig compatibility and project version gates..."
python3 "$ROOT/tools/test_ncv_kernel_config_compat.py"
python3 "$ROOT/tools/test_ncv_asset_gate.py"
echo "[NCV BUILD] Ensuring generated asset pipeline actually runs..."
# Remove only generated build outputs, not source/build caches. This forces Gradle to
# regenerate and re-merge APK assets while allowing already-built kernel/BusyBox outputs to be reused.
rm -rf "$ROOT/app/build/generated/ncvAssets/main" "$ROOT/app/build/intermediates/merged_assets/debug"
# AGP 8.x commonly stores mergeDebugAssets output under intermediates/assets/debug/mergeDebugAssets/out.
# Remove variant merge outputs too, without touching source assets or native/kernel build outputs.
if [[ -d "$ROOT/app/build/intermediates" ]]; then
  find "$ROOT/app/build/intermediates" -type d -name mergeDebugAssets -prune -exec rm -rf {} +
fi
rm -f "$APK"
"${GRADLE[@]}" :app:prepareNcvApkAssets

KERNEL_OUT="$ROOT/app/build/generated/ncvAssets/main/ncv/guest/diagnostic/Image"
INITRD_OUT="$ROOT/app/build/generated/ncvAssets/main/ncv/guest/diagnostic/initramfs.cpio.gzip"
MANIFEST_OUT="$ROOT/app/build/generated/ncvAssets/main/ncv/guest/manifest.json"
for artifact in "$KERNEL_OUT" "$INITRD_OUT" "$MANIFEST_OUT"; do
  if [[ ! -s "$artifact" ]]; then
    echo "ERROR: Gradle did not generate required APK asset: $artifact" >&2
    echo "Inspect :app:buildNcvDiagnosticKernel, :app:buildNcvStaticBusyBox, and :app:buildNcvTestInitramfs task output." >&2
    exit 10
  fi
done

echo "[NCV BUILD] Generated assets exist; explicitly merging the debug assets..."
"${GRADLE[@]}" :app:mergeDebugAssets

# The previous runtime reported a 0.1.18 libncv.so even though newer Kotlin/UI sources were present.
# Remove Android Gradle Plugin native intermediates so CMake must rebuild the C++ engine for this version.
rm -rf "$ROOT/app/.cxx" \
       "$ROOT/app/build/intermediates/cxx" \
       "$ROOT/app/build/intermediates/merged_native_libs" \
       "$ROOT/app/build/intermediates/stripped_native_libs"

# Use the AAPT-safe .gzip asset name; a final .gz suffix can be transformed by Android asset packaging.
for rel in \
  "ncv/guest/diagnostic/Image" \
  "ncv/guest/diagnostic/initramfs.cpio.gzip" \
  "ncv/guest/manifest.json"; do
  merged="$(find "$ROOT/app/build/intermediates" -type f -path "*/$rel" -print -quit 2>/dev/null || true)"
  if [[ -z "$merged" ]]; then
    echo "ERROR: AGP mergeDebugAssets output is missing '$rel' although generated assets exist." >&2
    echo "Merge-output files with NCV paths:" >&2
    find "$ROOT/app/build/intermediates" -type f -path '*/ncv/guest/*' -print 2>/dev/null | tail -40 >&2 || true
    exit 14
  fi
  case "$rel" in
    ncv/guest/diagnostic/Image) source="$KERNEL_OUT" ;;
    ncv/guest/diagnostic/initramfs.cpio.gzip) source="$INITRD_OUT" ;;
    ncv/guest/manifest.json) source="$MANIFEST_OUT" ;;
  esac
  if ! cmp -s "$source" "$merged"; then
    echo "ERROR: AGP merged '$rel' but its bytes differ from the freshly generated asset." >&2
    echo "Merged path: $merged" >&2
    exit 15
  fi
done

echo "[NCV BUILD] Merged assets confirmed; building APK..."
"${GRADLE[@]}" :app:assembleDebug
if [[ ! -s "$APK" ]]; then
  echo "ERROR: APK not found after build: $APK" >&2
  exit 11
fi
"$ROOT/tools/verify_ncv_super_bringup_apk.sh" "$APK"
if [[ "$INSTALL" -eq 1 ]]; then
  command -v adb >/dev/null 2>&1 || { echo "ERROR: adb is required for --install." >&2; exit 12; }
  adb get-state >/dev/null 2>&1 || { echo "ERROR: no connected Android device/emulator. Connect one and allow USB debugging." >&2; exit 13; }
  adb install -r "$APK"
  echo "Installed NCV 0.1.24 LLVM path-autodetection build. Confirm the app reports the 0.1.24 native engine marker."
fi
