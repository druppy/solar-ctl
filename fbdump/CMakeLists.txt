# fbdump - on-device framebuffer screenshot as PNG (zero deps beyond libc).
#
# Native build:  cmake -B build && cmake --build build
# Yocto (solar-fbdump recipe in fw/): same sources, cross-compiled; the
# FBDUMP_* cache variables below are the device defaults baked into the
# binary - CLI flags (-d/-g/-o) still override them at runtime.
cmake_minimum_required(VERSION 3.16)
project(solar-fbdump VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Device defaults (the "adapt to our setup" knobs) ----------------------
# Geometry/bpp are only the FALLBACK if FBIOGET_VSCREENINFO fails; at runtime
# the real fb geometry always wins over these. The device path is the plain
# default argument. Change them here (or -D... on the cmake command line)
# when the panel setup changes.
set(FBDUMP_FB_DEV "/dev/fb0" CACHE STRING "default framebuffer device")
set(FBDUMP_WIDTH  "142"      CACHE STRING "fallback framebuffer width")
set(FBDUMP_HEIGHT "428"      CACHE STRING "fallback framebuffer height")
set(FBDUMP_BPP    "16"       CACHE STRING "fallback bits per pixel (16=RGB565, 24, 32=XRGB8888, 8=gray)")

option(FBDUMP_BUILD_TESTS "Build Catch2 unit tests (native dev/CI only; keep OFF for Yocto)" OFF)

add_executable(fbdump src/main.cpp)
target_compile_definitions(fbdump PRIVATE
    FBDUMP_DEV="${FBDUMP_FB_DEV}"
    FBDUMP_W=${FBDUMP_WIDTH}
    FBDUMP_H=${FBDUMP_HEIGHT}
    FBDUMP_BPP=${FBDUMP_BPP})
target_compile_options(fbdump PRIVATE -Wall -Wextra)

include(GNUInstallDirs)
install(TARGETS fbdump RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(FBDUMP_BUILD_TESTS)
    enable_testing()
    # Catch2 from the platform (apt libcatch2-dev on CI runners and dev
    # laptops), never a fetched copy: find_package fails loudly when absent.
    find_package(Catch2 3 REQUIRED)

    # test_png.cpp #includes ../src/main.cpp on purpose (reaches the anonymous
    # namespace; FBDUMP_TESTS_BUILD compiles its main() out - do NOT also
    # compile src/main.cpp into this target, duplicate symbols).
    add_executable(fbdump_tests tests/test_png.cpp)
    target_include_directories(fbdump_tests PRIVATE tests)
    target_compile_definitions(fbdump_tests PRIVATE
        FBDUMP_TESTS_BUILD=1
        FBDUMP_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(fbdump_tests PRIVATE Catch2::Catch2WithMain)

    # Debian ships Catch.cmake next to Catch2Config.cmake; upstream puts it in
    # the config dir too, so ${Catch2_DIR} covers both.
    list(APPEND CMAKE_MODULE_PATH ${Catch2_DIR})
    include(Catch)
    catch_discover_tests(fbdump_tests)
endif()
