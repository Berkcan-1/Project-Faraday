# NCV 0.1.24 — LLVM path autodetection for Rust Binder

## Failure fixed

NCV 0.1.23 invoked the Rust preflight with `LLVM=1`. That works only when the process can find `clang` on `PATH`. The user could run `make ... LLVM="$LLVM_PREFIX/bin/" rustavailable` in an interactive terminal, but Gradle's child process still printed `clang: No such file or directory` and failed to find `bindgen`.

## Change

- Added `tools/ncv_llvm_toolchain.sh`, sourced by the kernel build helper.
- Autodetects a complete package under `~/toolchains/ncv/*`, `~/.local/opt/llvm-*`, or `/opt/llvm*`; `NCV_LLVM_TOOLCHAIN_DIR` and `LLVM_PREFIX` have priority.
- Adds the package's `bin/` to `PATH`, sets `LIBCLANG_PATH`, and passes an explicit `LLVM=/absolute/path/bin/` Kbuild argument.
- Applies that same resolved value to `rustavailable`, `defconfig`, `merge_config.sh`, `olddefconfig`, and `Image`.
- Adds the resolver to Gradle task inputs and kernel build signature to invalidate stale output when the resolver changes.
- Adds a regression test that creates a fake bundled toolchain under a temporary HOME while PATH deliberately excludes it.

## Scope

These source-level tests are not a real kernel build and are not proof of Android userspace boot. The goal is to fix the exact PATH mismatch and ensure Gradle can discover the tools, then continue to the actual kernel build.
