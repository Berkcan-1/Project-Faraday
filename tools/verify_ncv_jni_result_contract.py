#!/usr/bin/env python3
"""Static contract check for NCV's Android boot JNI result array."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
cpp = (root / "app/src/main/cpp/jni/ncv_jni.cpp").read_text()
kotlin = (root / "app/src/main/java/com/aerolon/ncv/NcvVm.kt").read_text()
checks = {
    "single result count constant": r"constexpr\s+jsize\s+kAndroidBootResultCount\s*=\s*6\s*;",
    "std::array sized by result count": r"std::array\s*<\s*jlong\s*,\s*kAndroidBootResultCount\s*>\s+values",
    "JNI array allocation uses result count": r"NewLongArray\s*\(\s*kAndroidBootResultCount\s*\)",
    "JNI copy uses result count and data pointer": r"SetLongArrayRegion\s*\(\s*result\s*,\s*0\s*,\s*kAndroidBootResultCount\s*,\s*values\.data\(\)\s*\)",
    "embedded DTB field occupies slot 5": r"values\s*\[\s*5\s*\]\s*=\s*static_cast<jlong>\(info\.embeddedDtbSize\)",
}
failed = False
for name, pattern in checks.items():
    ok = re.search(pattern, cpp) is not None
    print(("PASS" if ok else "FAIL") + ": " + name)
    failed |= not ok
kotlin_ok = "result.size < 6" in kotlin and "result[5]" in kotlin
print(("PASS" if kotlin_ok else "FAIL") + ": Kotlin expects the same six-field return layout")
failed |= not kotlin_ok
# Ensure the old unsafe declaration has not returned.
unsafe = re.search(r"jlong\s+values\s*\[\s*5\s*\]", cpp) is not None
print(("FAIL" if unsafe else "PASS") + ": no five-element C array for six-field JNI result")
failed |= unsafe
sys.exit(1 if failed else 0)
