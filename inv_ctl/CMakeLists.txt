# inv_ctl - solar-ctl inverter controller UI (LVGL + glibmm).
#
# Two supported UI build flavors:
#   native (laptop):  cmake -B build && cmake --build build   (SDL2 window;
#                     LVGL v9.4.0 fetched pinned to Yocto's SRCREV)
#   Yocto (inv-ctl recipe): -DINV_CTL_SYSTEM_LVGL=ON, links the packaged
#                     lvgl (fbdev build) from the sysroot.
# Unit tests (system Catch2, native/CI only - the Yocto build never turns
# them on; swupdate protocol headers always required, see the block below):
#   cmake -B b -DINV_CTL_BUILD_UI=OFF -DINV_CTL_BUILD_TESTS=ON \
#         -DINV_CTL_SWUPDATE_INCLUDE=<swupdate-src>/include
#   ctest --test-dir b
cmake_minimum_required(VERSION 3.16)
project(inv_ctl VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(INV_CTL_SYSTEM_LVGL "Link system lvgl (Yocto sysroot) instead of fetching v9.4.0" OFF)
option(INV_CTL_SYSTEM_SWUPDATE "Use swupdate headers + libswupdate from sysroot (Yocto) instead of fetched headers" OFF)
option(INV_CTL_BUILD_UI "Build the inv-ctl executable (needs glibmm + lvgl)" ON)
option(INV_CTL_BUILD_TESTS "Build Catch2 unit tests (native dev/CI only; keep OFF for Yocto)" OFF)

# Dependency policy: everything except LVGL must already exist on the
# platform - a missing dep is a hard configure error, never a silent
# download. LVGL is fetched (native only) precisely because of its
# embedded nature; swupdate and Catch2 are ordinary platform packages.

# swupdate protocol definitions (progress_ipc.h et al) always come from
# swupdate itself - never a hand copy:
#   Yocto:  swupdate-dev in the sysroot (DEPENDS in the inv-ctl recipe)
#           plus libswupdate, whose get_prog_socket() we then call.
#   native/CI: point -DINV_CTL_SWUPDATE_INCLUDE=<dir> at the include/ tree
#           of a swupdate checkout, or install its headers so a plain
#           search finds progress_ipc.h. swupdate is a kbuild project:
#           NEVER add_subdirectory/FetchContent-MakeAvailable it.
if(INV_CTL_BUILD_UI OR INV_CTL_BUILD_TESTS)
    find_path(INV_CTL_SWUPDATE_INCLUDE progress_ipc.h
              PATHS ENV SWUPDATE_SRC
              PATH_SUFFIXES include include/swupdate swupdate)
    if(NOT INV_CTL_SWUPDATE_INCLUDE)
        message(FATAL_ERROR
            "swupdate protocol headers not found (progress_ipc.h). Either\n"
            "  -DINV_CTL_SWUPDATE_INCLUDE=<dir>   with <dir> = the include/ tree of a\n"
            "      swupdate checkout at the SRCREV this build must match\n"
            "      (target build: the version meta-swupdate provides), or\n"
            "  export SWUPDATE_SRC=<swupdate-source-root>\n"
            "git clone --depth 1 --branch 2026.05.1 https://github.com/sbabic/swupdate.git\n"
            "works for the version the target currently runs.")
    endif()
    message(STATUS "swupdate protocol headers: ${INV_CTL_SWUPDATE_INCLUDE}")
endif()
if(INV_CTL_SYSTEM_SWUPDATE)
    # The target also links libswupdate itself: get_prog_socket() resolution
    # must not drift from the daemon's.
    find_library(INV_CTL_SWUPDATE_LIB NAMES swupdate REQUIRED)
endif()

if(INV_CTL_BUILD_UI)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLIBMM REQUIRED IMPORTED_TARGET glibmm-2.68)

    # src/ = data + protocol, src/ui/ = everything that renders.
    add_executable(inv-ctl
        src/main.cpp
        src/net.cpp
        src/board.cpp
        src/update.cpp
        src/updater.cpp
        src/ui/backend.cpp
        src/ui/watch.cpp
        src/ui/status_panel.cpp
        src/ui/upgrade.cpp)
    # main.cpp includes "ui/<x>.hpp"; src/ui files include "net.hpp" etc.
    target_include_directories(inv-ctl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(inv-ctl PRIVATE $<$<BOOL:${INV_CTL_SYSTEM_LVGL}>:INV_CTL_SYSTEM_LVGL=1>)
    target_link_libraries(inv-ctl PRIVATE PkgConfig::GLIBMM)

    # progress IPC: upstream headers always; on the target also the library
    # itself (get_prog_socket() resolution cannot drift from the daemon).
    target_include_directories(inv-ctl PRIVATE ${INV_CTL_SWUPDATE_INCLUDE})
    if(INV_CTL_SYSTEM_SWUPDATE)
        target_compile_definitions(inv-ctl PRIVATE INV_CTL_SYSTEM_SWUPDATE=1)
        target_link_libraries(inv-ctl PRIVATE ${INV_CTL_SWUPDATE_LIB})
    endif()

    if(INV_CTL_SYSTEM_LVGL)
        # lvgl packaged by meta-oe (Kconfig build): headers land in
        # ${includedir}/lvgl/ together with the expanded lv_conf.h, which the
        # __has_include() logic in lv_conf_internal.h picks up automatically.
        find_path(LVGL_INCLUDE_DIR lvgl.h REQUIRED PATH_SUFFIXES lvgl)
        find_library(LVGL_LIB NAMES lvgl REQUIRED)
        target_include_directories(inv-ctl PRIVATE ${LVGL_INCLUDE_DIR})
        target_link_libraries(inv-ctl PRIVATE ${LVGL_LIB})
    else()
        include(FetchContent)
        # Keep the knobs off that meta-oe's defconfig also turns off.
        set(CONFIG_LV_BUILD_DEMOS   OFF CACHE BOOL "" FORCE)
        set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(LV_BUILD_USE_KCONFIG    OFF CACHE BOOL "" FORCE)
        set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/native/lv_conf.h" CACHE PATH "" FORCE)
        FetchContent_Declare(lvgl
            GIT_REPOSITORY https://github.com/lvgl/lvgl.git
            # v9.4.0 - identical to SRCREV of meta-oe lvgl_9.4.0.bb (wrynose)
            GIT_TAG c016f72d4c125098287be5e83c0f1abed4706ee5
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(lvgl)
        target_link_libraries(inv-ctl PRIVATE lvgl::lvgl)
        target_include_directories(inv-ctl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/native)
        target_compile_definitions(inv-ctl PRIVATE LV_CONF_INCLUDE_SIMPLE)

        # LVGL compiles but does not link its SDL driver; the app owns SDL2.
        find_package(SDL2 REQUIRED)
        target_link_libraries(inv-ctl PRIVATE SDL2::SDL2)
    endif()

    install(TARGETS inv-ctl RUNTIME DESTINATION bin)
endif()

if(INV_CTL_BUILD_TESTS)
    enable_testing()
    # Catch2 from the platform (apt libcatch2-dev on CI runners and dev
    # laptops), never a fetched copy: find_package fails loudly when absent.
    find_package(Catch2 3 REQUIRED)

    # test_net.cpp #includes ../src/net.cpp on purpose (reaches the anonymous
    # namespace; do NOT also compile src/net.cpp into this target - duplicate
    # symbols). Same for test_board.cpp and src/board.cpp, and for
    # test_update.cpp and src/update.cpp (libc-only by design, so the tests
    # need neither glibmm nor LVGL).
    add_executable(inv_ctl_tests
        tests/test_net.cpp
        tests/test_board.cpp
        tests/test_update.cpp)
    target_include_directories(inv_ctl_tests PRIVATE
        src ${INV_CTL_SWUPDATE_INCLUDE})
    target_link_libraries(inv_ctl_tests PRIVATE Catch2::Catch2WithMain)

    # Debian ships Catch.cmake next to Catch2Config.cmake; upstream puts it in
    # the config dir too, so ${Catch2_DIR} covers both.
    list(APPEND CMAKE_MODULE_PATH ${Catch2_DIR})
    include(Catch)
    catch_discover_tests(inv_ctl_tests)
endif()
