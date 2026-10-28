# NCV 0.1.23 — LLVM/Rust Binder build fix

## Why 0.1.22 stopped

The user's kernel build detected the modern `CONFIG_ANDROID_BINDER_IPC_RUST` symbol and attempted to select `CONFIG_RUST=y`, but the final `.config` left both Rust and Rust Binder disabled. The log reported `CONFIG_RUSTC_VERSION` and Rust/LLVM version metadata, but those values alone do not mean the kernel's `RUST_IS_AVAILABLE` predicate passed.

The 0.1.22 build command used the AArch64 GCC toolchain without the kernel's LLVM build mode. Upstream Linux documents `make LLVM=1 rustavailable` as the direct diagnostic for why Rust support can be enabled or disabled. See the [kernel Rust quick-start guide](https://docs.kernel.org/next/rust/quick-start.html).

## Changes in 0.1.23

- Detect Rust Binder before configuring the kernel.
- For Rust Binder trees, run `make -C "$KERNEL_SRC" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" LLVM=1 rustavailable` and stop if it fails, preserving the actual toolchain diagnosis.
- Pass `LLVM=1` to `defconfig`, the nested `merge_config.sh`/`olddefconfig` path, and the final `Image` compile so Kconfig and Kbuild use the same toolchain mode.
- Keep the GCC path for legacy kernel trees that provide `CONFIG_ANDROID_BINDER_IPC`.
- Update native engine marker to `0.1.23-dev-llvm-rust-binder-20261028` and increment the app version code to 11.
- Add regression assertions ensuring Rust Binder builds do not silently omit LLVM mode or the `rustavailable` check.

## Re-test

Run from the project root:

```bash
./tools/build_verify_ncv_super_bringup.sh --install
```

If it stops during the `rustavailable` preflight, install the Rust toolchain versions required by the specific kernel tree, including Rust standard-library sources (`rust-src`), `bindgen`, and libclang/LLVM, then retry. The kernel's own diagnostic output is the authority for the missing requirement. Do not just force `CONFIG_RUST=y` in `.config`; Kconfig will continue to disable it when its dependencies are unavailable.

If the kernel compiles, the next runtime milestones are UART output and VirtIO register reads, followed by actual Android first-stage init/mount logs. Build success is not Android boot success.

## Validation in the authoring environment

Static compatibility fixtures, asset/version gates, and shell syntax can be tested here. This environment does not have the user's `/home/berkcan/src/linux` tree and cannot certify the actual LLVM/Rust kernel build or device boot.
