#!/usr/bin/env python3
"""Test Kconfig compatibility selection for old and current mainline-style kernel trees."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "tools/prepare_ncv_kernel_config_fragment.sh"
BASE = ROOT / "guest-board/ncv_linux_bringup.config"


def write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)


def run_helper(kernel: Path, output: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["bash", str(HELPER), str(kernel), str(BASE), str(output)],
        check=False, text=True, capture_output=True,
    )


with tempfile.TemporaryDirectory(prefix="ncv-kconfig-test-") as td:
    tmp = Path(td)

    # Current-mainline layout: no CONFIG_DM_LINEAR, dm-linear.o is linked to dm-mod,
    # and only the Rust Binder symbol is available.
    modern = tmp / "modern"
    write(modern / "drivers/md/Kconfig", "config BLK_DEV_DM\n\ttristate \"Device mapper support\"\n")
    write(modern / "drivers/md/Makefile", "dm-mod-y += dm.o dm-table.o dm-linear.o\n")
    write(modern / "drivers/android/Kconfig", (
        "config ANDROID_BINDER_IPC_RUST\n\tbool \"Android Binder IPC Driver\"\n"
        "config ANDROID_BINDER_DEVICES\n\tstring \"Android Binder devices\"\n"
    ))
    modern_out = tmp / "modern.config"
    modern_result = run_helper(modern, modern_out)
    assert modern_result.returncode == 0, modern_result.stderr
    modern_fragment = modern_out.read_text()
    assert "CONFIG_DM_LINEAR=y" not in modern_fragment
    assert "CONFIG_RUST=y" in modern_fragment
    assert "CONFIG_ANDROID_BINDER_IPC_RUST=y" in modern_fragment
    assert 'CONFIG_ANDROID_BINDER_DEVICES="binder,hwbinder,vndbinder"' in modern_fragment
    assert "CONFIG_ANDROID_BINDER_IPC=y" not in modern_fragment
    assert "CONFIG_ANDROID_BINDERFS=y" not in modern_fragment

    # Older layout: separate dm-linear symbol and legacy Binder/BinderFS exist.
    legacy = tmp / "legacy"
    write(legacy / "drivers/md/Kconfig", "config DM_LINEAR\n\ttristate \"Linear target\"\n")
    write(legacy / "drivers/md/Makefile", "obj-$(CONFIG_DM_LINEAR) += dm-linear.o\n")
    write(legacy / "drivers/android/Kconfig", (
        "config ANDROID_BINDER_IPC\n\tbool \"Android Binder IPC Driver\"\n"
        "config ANDROID_BINDERFS\n\tbool \"Android Binderfs filesystem\"\n"
        "config ANDROID_BINDER_DEVICES\n\tstring \"Android Binder devices\"\n"
    ))
    legacy_out = tmp / "legacy.config"
    legacy_result = run_helper(legacy, legacy_out)
    assert legacy_result.returncode == 0, legacy_result.stderr
    legacy_fragment = legacy_out.read_text()
    assert "CONFIG_DM_LINEAR=y" in legacy_fragment
    assert "CONFIG_ANDROID_BINDER_IPC=y" in legacy_fragment
    assert "CONFIG_ANDROID_BINDERFS=y" in legacy_fragment
    assert "CONFIG_ANDROID_BINDER_IPC_RUST=y" not in legacy_fragment

    # Fail loudly if neither a Kconfig symbol nor a built-in dm-linear object exists.
    broken = tmp / "broken"
    write(broken / "drivers/md/Kconfig", "config BLK_DEV_DM\n\ttristate \"Device mapper support\"\n")
    write(broken / "drivers/md/Makefile", "obj-$(CONFIG_BLK_DEV_DM) += dm-mod.o\n")
    write(broken / "drivers/android/Kconfig", "config ANDROID_BINDER_IPC\n\tbool \"Android Binder IPC Driver\"\n")
    broken_result = run_helper(broken, tmp / "broken.config")
    assert broken_result.returncode == 4, (broken_result.returncode, broken_result.stderr)

config = BASE.read_text()
for required in (
    "CONFIG_AUDIT=y", "CONFIG_NET=y", "CONFIG_INET=y",
    "CONFIG_SECURITY_NETWORK=y", "CONFIG_SECURITY_SELINUX=y",
):
    assert required in config, f"missing SELinux dependency: {required}"
assert "CONFIG_DM_LINEAR=y" not in config, "DM_LINEAR must be selected only by the compatibility helper"
assert "CONFIG_ANDROID_BINDER_IPC=y" not in config, "Binder choice must be kernel-version-aware"
assert "CONFIG_ANDROID_BINDERFS=y" not in config, "BinderFS choice must be kernel-version-aware"
print("PASS: modern Rust Binder and built-in dm-linear layout")
print("PASS: legacy Binder/BinderFS and standalone dm-linear layout")
print("PASS: unsupported dm-linear layout fails with a clear error")
print("PASS: SELinux Kconfig dependencies are explicitly requested")

# Modern Rust Binder must use a real toolchain path inside the Gradle child
# process, not rely on an interactive terminal's PATH/LLVM exports.
builder = (ROOT / "tools/build_ncv_kernel.sh").read_text()
toolchain_helper = ROOT / "tools/ncv_llvm_toolchain.sh"
assert 'source "$ROOT/tools/ncv_llvm_toolchain.sh"' in builder
assert 'ncv_configure_llvm_toolchain' in builder
assert 'LLVM_ARGS=("LLVM=$NCV_LLVM_SETTING")' in builder
assert '"${LLVM_ARGS[@]}" rustavailable' in builder
assert 'LLVM="$NCV_LLVM_SETTING" "$KERNEL_SRC/scripts/kconfig/merge_config.sh"' in builder
assert '"${LLVM_ARGS[@]}" O="$OUT_DIR" defconfig' in builder
assert '"${LLVM_ARGS[@]}" O="$OUT_DIR" olddefconfig' in builder
assert '"${LLVM_ARGS[@]}" O="$OUT_DIR" -j"$JOBS" Image' in builder
assert 'candidates+=("$HOME"/toolchains/ncv/*)' in toolchain_helper.read_text()
assert 'export PATH="$NCV_LLVM_BIN_DIR:$HOME/.cargo/bin:$PATH"' in toolchain_helper.read_text()

# Exercise discovery in a fake, isolated HOME with a deliberately minimal PATH,
# exactly like a Gradle daemon that did not inherit an interactive export.
with tempfile.TemporaryDirectory(prefix="ncv-llvm-autodetect-") as td:
    fake_home = Path(td)
    toolroot = fake_home / "toolchains/ncv/llvm-test"
    (toolroot / "bin").mkdir(parents=True)
    (toolroot / "lib").mkdir()
    for tool in ("clang", "bindgen"):
        exe = toolroot / "bin" / tool
        exe.write_text("#!/bin/sh\nexit 0\n")
        exe.chmod(0o755)
    (toolroot / "lib/libclang.so").write_text("fixture")
    probe = subprocess.run(
        ["bash", "-c", 'source "$1"; ncv_configure_llvm_toolchain >/dev/null; printf "%s\n%s\n%s\n" "$NCV_LLVM_SETTING" "$LIBCLANG_PATH" "$(command -v bindgen)"', "ncv-test", str(toolchain_helper)],
        env={"HOME": str(fake_home), "PATH": "/usr/bin:/bin"},
        check=False, text=True, capture_output=True,
    )
    assert probe.returncode == 0, probe.stderr
    resolved = probe.stdout.splitlines()
    assert resolved[0] == str(toolroot / "bin") + "/", resolved
    assert resolved[1] == str(toolroot / "lib"), resolved
    assert resolved[2] == str(toolroot / "bin/bindgen"), resolved

print("PASS: Rust Binder toolchain autodetection works without inherited interactive PATH")
