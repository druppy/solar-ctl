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
    include(FetchContent)
    # Same Catch2 pin as inv_ctl's unit tests (EXCLUDE_FROM_ALL needs
    # CMake>=3.28; the CI runner provides it, the Yocto build never
    # configures this branch - FBDUMP_BUILD_TESTS stays OFF there).
    FetchContent_Declare(catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(catch2)

    # test_png.cpp #includes ../src/main.cpp on purpose (reaches the anonymous
    # namespace; FBDUMP_TESTS_BUILD compiles its main() out - do NOT also
    # compile src/main.cpp into this target, duplicate symbols).
    add_executable(fbdump_tests
        tests/test_png.cpp
        ${catch2_SOURCE_DIR}/extras/catch_amalgamated.cpp)
    target_include_directories(fbdump_tests PRIVATE
        tests ${catch2_SOURCE_DIR}/extras)
    target_compile_definitions(fbdump_tests PRIVATE
        FBDUMP_TESTS_BUILD=1
        FBDUMP_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")

    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(fbdump_tests)
endif()
