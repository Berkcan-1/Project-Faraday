# NCV JNI stack-corruption fix — 2026-10-27

## Root cause

The native method `Java_com_aerolon_ncv_NativeBridge_nativeLoadAndroidBootBundle` had a six-field return contract after embedded-DTB reporting was added, but retained a five-element local array:

- Declaration: `jlong values[5]`
- New field: `values[5] = info.embeddedDtbSize`
- JNI return: `NewLongArray(6)` and `SetLongArrayRegion(..., 6, values)`

Writing `values[5]` wrote one `jlong` beyond the stack array. On the user's ARM64 Android device, the write corrupted stack-protector data and triggered `SIGABRT` with `stack corruption detected (-fstack-protector)`. This explains the crash backtrace at `nativeLoadAndroidBootBundle+1460`.

## Fix applied

- Replaced the local C array with `std::array<jlong, kAndroidBootResultCount>`.
- Defined the result count once as six and reused it in both `NewLongArray` and `SetLongArrayRegion`.
- Preserved the existing six-field Kotlin contract: status, entry, initrd address, initrd size, boot header version, embedded DTB size.
- Updated the native engine build marker to `0.1.4-dev-jni-stackfix-20261027`.
- Added `tools/verify_ncv_jni_result_contract.py` to guard the C++/Kotlin array contract against a recurrence.

## Validation performed here

- `g++ -std=c++17 -fsyntax-only` for `ncv_jni.cpp` against the installed JDK JNI headers: passed.
- JNI result-contract verification script: passed.
- Source search found no remaining `jlong values[5]` declaration in the JNI file.
- Android Gradle/NDK APK build and physical-device reproduction were not available in this environment. This is a source-level fix; the user must rebuild and validate on device.

## Rebuild and verify

From the Android Studio project root:

```bash
./tools/verify_ncv_jni_result_contract.py
./gradlew --stop
rm -rf app/.cxx app/build
./gradlew :app:assembleDebug --rerun-tasks
./tools/verify_ncv_revidr_apk.sh app/build/outputs/apk/debug/app-debug.apk
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Confirm the console reports `0.1.4-dev-jni-stackfix-20261027`. Then repeat the same boot attempt. This fix addresses the app crash during boot-bundle loading; it does not by itself make the Android guest boot successfully.
