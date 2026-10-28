# Host (Linux/macOS) üzerinde motor çekirdeği + birim testleri. Android derlemesi için app/src/main/cpp/CMakeLists.txt kullanılır.
cmake_minimum_required(VERSION 3.18)
project(ncv_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

set(NCV_SANITIZE "" CACHE STRING "Örn: address,undefined  veya  thread")
set(ENGINE ${CMAKE_SOURCE_DIR}/app/src/main/cpp)

add_library(ncv_engine STATIC
  ${ENGINE}/core/ncv_vm.cpp
  ${ENGINE}/cpu/ncv_cpu.cpp
  ${ENGINE}/cpu/ncv_fpsimd.cpp
  ${ENGINE}/cpu/ncv_asimd.cpp
  ${ENGINE}/cpu/ncv_asimd2.cpp
  ${ENGINE}/cpu/ncv_softfloat.cpp
  ${ENGINE}/memory/ncv_bus.cpp
  ${ENGINE}/devices/ncv_uart.cpp
  ${ENGINE}/devices/ncv_gicv2.cpp
  ${ENGINE}/devices/ncv_virtio_blk.cpp
  ${ENGINE}/devices/ncv_virtio_rng.cpp
  ${ENGINE}/devices/ncv_mtk_compat.cpp
  ${ENGINE}/devices/ncv_pl031_rtc.cpp
  ${ENGINE}/devices/ncv_framebuffer.cpp
  ${ENGINE}/loader/ncv_loader.cpp
  ${ENGINE}/loader/ncv_dtb.cpp
  ${ENGINE}/loader/ncv_linux_image.cpp
  ${ENGINE}/loader/ncv_android_boot_image.cpp
  ${ENGINE}/selftest/ncv_selftest.cpp)
target_compile_options(ncv_engine PUBLIC -Wall -Wextra)

if(NCV_SANITIZE)
  target_compile_options(ncv_engine PUBLIC -fsanitize=${NCV_SANITIZE} -fno-omit-frame-pointer -g)
  target_link_options(ncv_engine PUBLIC -fsanitize=${NCV_SANITIZE})
endif()

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
add_executable(ncv_tests
  native-tests/main.cpp
  native-tests/test_bus.cpp
  native-tests/test_uart.cpp
  native-tests/test_cpu.cpp
  native-tests/test_fp.cpp
  native-tests/test_asimd.cpp
  native-tests/test_asimd2.cpp
  native-tests/test_vm.cpp
  native-tests/test_loader.cpp
  native-tests/test_selftest.cpp
  native-tests/test_arch.cpp
  native-tests/test_stage4cd.cpp
  native-tests/test_android_boot.cpp
  native-tests/test_virtio_blk.cpp
  native-tests/test_virtio_rng.cpp
  native-tests/test_pl031_framebuffer.cpp
  native-tests/test_kernel_compat.cpp
  native-tests/test_mtk_compat.cpp
  native-tests/test_gicv2.cpp)
target_include_directories(ncv_tests PRIVATE ${ENGINE})
target_link_libraries(ncv_engine PUBLIC ZLIB::ZLIB)
target_link_libraries(ncv_tests ncv_engine Threads::Threads)

enable_testing()
add_test(NAME ncv_tests COMMAND ncv_tests)
