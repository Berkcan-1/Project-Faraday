#!/usr/bin/env python3
"""Static regression checks for the diagnostic Linux asset/build gate."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
version = (root / "app/src/main/cpp/core/ncv_version.h").read_text()
gradle = (root / "app/build.gradle.kts").read_text()
activity = (root / "app/src/main/java/com/aerolon/ncv/GuestConsoleActivity.kt").read_text()
verify = (root / "tools/verify_ncv_super_bringup_apk.sh").read_text()
local = (root / "local.properties").read_text()

marker = re.search(r'NCV_ENGINE_VERSION\s+"([^"]+)"', version)
assert marker, "native engine marker missing"
assert marker.group(1) == "0.1.24-dev-llvm-path-autodetect-20261028", marker.group(1)
assert 'versionName = "0.1.24-dev"' in gradle
assert 'versionCode = 12' in gradle
assert '"ncv.requireDiagnosticGuestArtifacts", "true"' in gradle
assert 'inputs.property("requireDiagnosticGuestArtifacts", ncvRequireDiagnosticGuestArtifacts)' in gradle
assert 'if (ncvPackageGuestArtifacts && ncvRequireDiagnosticGuestArtifacts && !(kernelIncluded && initramfsIncluded))' in gradle
assert 'private const val NCV_DIAGNOSTIC_KERNEL_ASSET = "ncv/guest/diagnostic/Image"' in activity
assert 'private const val NCV_DIAGNOSTIC_INITRD_ASSET = "ncv/guest/diagnostic/initramfs.cpio.gzip"' in activity
assert 'diagDir.resolve("initramfs.cpio.gzip")' in gradle
assert 'diagDir.resolve("initramfs.cpio.gz")' not in gradle
assert 'diagnosticGuestAssetsAvailable' in activity
assert 'startNcvSuperBringupButton.isEnabled = canRunDiagnostic' in activity
assert 'val canRunDiagnostic = canEditProfile && selectedSuperUri != null &&\n            diagnosticGuestAssetsAvailable && nativeEngineMatchesExpected()' in activity
assert 'startNcvSuperBringupButton.isEnabled = canEditProfile && selectedSuperUri != null' not in activity
assert 'Try Android userspace · NCV kernel + original ramdisk' in activity
assert 'startNcvKernelAndroidButton = NcvUi.button(this, "Try Android userspace · NCV kernel + original ramdisk →", primary = true)' in activity
assert 'startNcvSuperBringupButton = NcvUi.button(this, "Storage test only · generic Linux (not Android) →", primary = false)' in activity
assert activity.index('startNcvKernelAndroidButton = NcvUi.button(this, "Try Android userspace · NCV kernel + original ramdisk →", primary = true)') < activity.index('startNcvSuperBringupButton = NcvUi.button(this, "Storage test only · generic Linux (not Android) →", primary = false)')
assert 'private var selectedCompatProfileId: Int = 1' in activity
assert 'private var selectedDtbModeId: Int = 1' in activity
assert 'Effective kernel command line: $dtbBootArgs' in activity
assert activity.rfind('startNcvSuperBringupButton.isEnabled = canRunDiagnostic') > activity.find('private fun refreshBootProfileSummary')
assert '0.1.24-dev-llvm-path-autodetect-20261028' in verify
assert 'ncv.requireDiagnosticGuestArtifacts=true' in local
assert not re.search(r"^sdk\.dir=.*ncv\.requireDiagnosticGuestArtifacts=true$", local, re.MULTILINE)
build_script = (root / "tools/build_verify_ncv_super_bringup.sh").read_text()
assert "prepareNcvApkAssets" in build_script
assert ':app:mergeDebugAssets' in build_script
assert 'tasks.matching { it.name == "mergeDebugAssets" }.configureEach' in gradle
assert "AGP mergeDebugAssets output is missing" in build_script
assert "ncv/guest/diagnostic/initramfs.cpio.gzip" in build_script
assert "ncv/guest/diagnostic/initramfs.cpio.gz\"" not in build_script
assert "ncvKernelImage" not in build_script or "Image" in build_script
assert "nativeExtractAndroidInitramfs" in (root / "app/src/main/java/com/aerolon/ncv/NativeBridge.kt").read_text()
assert "extractAndroidInitramfs" in (root / "app/src/main/cpp/loader/ncv_android_boot_image.cpp").read_text()
assert '"$ROOT/app/.cxx"' in build_script
assert 'startAndroidBootAttempt(useNcvKernel = true)' in activity
assert 'activeVm.loadLinuxImage(kernel)' in activity
assert 'activeVm.loadPayload(ramdisk, initrdAddress)' in activity
kernel_builder = (root / "tools/build_ncv_kernel.sh").read_text()
compat_helper = (root / "tools/prepare_ncv_kernel_config_fragment.sh").read_text()
board_config = (root / "guest-board/ncv_linux_bringup.config").read_text()
assert "prepare_ncv_kernel_config_fragment.sh" in kernel_builder
assert "CONFIG_ANDROID_BINDER_IPC_RUST" in compat_helper
assert "CONFIG_DM_LINEAR" in compat_helper
assert "CONFIG_SECURITY_NETWORK" in board_config
print("PASS: Android init path, native version marker, clean native rebuild, and UI gates are consistent")
