# NCV yerel motoru (Android Studio / NDK). Gradle'dan externalNativeBuild ile çağrılır.
cmake_minimum_required(VERSION 3.18)
project(ncv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Motor çekirdeği: Android'e bağımlı DEĞİL (host testleri kökteki CMakeLists.txt ile bunu kullanır).
set(NCV_ENGINE_SOURCES
    core/ncv_vm.cpp
    cpu/ncv_cpu.cpp
    cpu/ncv_fpsimd.cpp
    cpu/ncv_asimd.cpp
    cpu/ncv_asimd2.cpp
    cpu/ncv_softfloat.cpp
    memory/ncv_bus.cpp
    devices/ncv_uart.cpp
    devices/ncv_gicv2.cpp
    devices/ncv_virtio_blk.cpp
    devices/ncv_virtio_rng.cpp
    devices/ncv_mtk_compat.cpp
    devices/ncv_pl031_rtc.cpp
    devices/ncv_framebuffer.cpp
    loader/ncv_loader.cpp
    loader/ncv_dtb.cpp
    loader/ncv_linux_image.cpp
    loader/ncv_android_boot_image.cpp
    selftest/ncv_selftest.cpp)

add_library(ncv SHARED ${NCV_ENGINE_SOURCES} jni/ncv_jni.cpp)
target_compile_options(ncv PRIVATE -Wall -Wextra -fvisibility=hidden)
find_library(log-lib log)
target_link_libraries(ncv ${log-lib} z)
