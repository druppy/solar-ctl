/* Include shim: LVGL comes from the sysroot in Yocto builds (system
 * install layout ${includedir}/lvgl/) and from the FetchContent source
 * tree natively (include dir = lvgl root). */
#pragma once

#if INV_CTL_SYSTEM_LVGL
#include <lvgl/lvgl.h>
#else
#include "lvgl.h"
#endif
