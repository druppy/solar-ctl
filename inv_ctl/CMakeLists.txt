# inv_ctl - solar-ctl inverter controller UI (LVGL + glibmm).
#
# Two supported UI build flavors:
#   native (laptop):  cmake -B build && cmake --build build   (SDL2 window;
#                     LVGL v9.4.0 fetched pinned to Yocto's SRCREV)
#   Yocto (inv-ctl recipe): -DINV_CTL_SYSTEM_LVGL=ON, links the packaged
#                     lvgl (fbdev build) from the sysroot.
# Unit tests (Catch2, native/CI only - the Yocto build never turns them on):
#   cmake -B b -DINV_CTL_BUILD_UI=OFF -DINV_CTL_BUILD_TESTS=ON && ctest --test-dir b
cmake_minimum_required(VERSION 3.16)
project(inv_ctl VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(INV_CTL_SYSTEM_LVGL "Link system lvgl (Yocto sysroot) instead of fetching v9.4.0" OFF)
option(INV_CTL_BUILD_UI "Build the inv-ctl executable (needs glibmm + lvgl)" ON)
option(INV_CTL_BUILD_TESTS "Build Catch2 unit tests (native dev/CI only; keep OFF for Yocto)" OFF)

if(INV_CTL_BUILD_UI)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLIBMM REQUIRED IMPORTED_TARGET glibmm-2.68)

    add_executable(inv-ctl
        src/main.cpp
        src/watch.cpp
        src/net.cpp
        src/status_panel.cpp
        src/backend.cpp)
    target_compile_definitions(inv-ctl PRIVATE $<$<BOOL:${INV_CTL_SYSTEM_LVGL}>:INV_CTL_SYSTEM_LVGL=1>)
    target_link_libraries(inv-ctl PRIVATE PkgConfig::GLIBMM)

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
    include(FetchContent)
    # Amalgamated Catch2 v3 (single .cpp + .h from extras/): no library build,
    # tests configure in seconds - the whole point of a fast parallel CI job.
    FetchContent_Declare(catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL)   # only extras/ modules + amalgamated .cpp are used
    FetchContent_MakeAvailable(catch2)

    # test_net.cpp #includes ../src/net.cpp on purpose (reaches the anonymous
    # namespace; do NOT also compile src/net.cpp into this target - duplicate
    # symbols).
    add_executable(inv_ctl_tests
        tests/test_net.cpp
        ${catch2_SOURCE_DIR}/extras/catch_amalgamated.cpp)
    target_include_directories(inv_ctl_tests PRIVATE
        src ${catch2_SOURCE_DIR}/extras)

    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(inv_ctl_tests)
endif()
