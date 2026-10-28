# NCV APK Asset Name Fix (v0.1.18)

## Root cause

The kernel and BusyBox initramfs build tasks succeeded, and Gradle generated both guest assets. The APK still failed verification because Android asset packaging treats a file whose final suffix is `.gz` specially: the gzip payload may be gunzipped and the `.gz` suffix removed. The user's AGP intermediate output showed `initramfs.cpio` rather than `initramfs.cpio.gz`, proving the build had transformed the name before final APK verification.

## Fix

- The build-generated source remains `ncv-test-initramfs.cpio.gz`, and its contents remain a gzip-compressed `newc` CPIO archive.
- The generated APK asset is now named `ncv/guest/diagnostic/initramfs.cpio.gzip`. The final suffix is deliberately not `.gz`, so AAPT will not apply the special gzip handling.
- The manifest stores the SHA-256 and byte length of the original gzip payload under the new asset path.
- `MainActivity`, `GuestConsoleActivity`, the build/merge script, and the APK verifier all use the same `.gzip` asset path. The verifier still decompresses the extracted bytes during validation, proving that the actual payload remains gzip.
- Version marker: `0.1.18-dev-ncv-gzip-asset-name-fix-20261028` (`versionCode` 6).

## Verification performed here

- The static asset gate regression test passes.
- Both shell scripts pass `bash -n`.
- A synthetic APK with the expected native marker, ARM64 Image magic, manifest hashes, and gzip initramfs passes the full APK verification script.

A real APK build/install and actual guest-kernel boot must still be verified on the developer's machine. This fix addresses asset packaging; it does not by itself prove Linux or Android boot.
