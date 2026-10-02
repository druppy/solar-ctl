#pragma once

#include "lvgl.hpp"

/* Analog watch face, sized to fit the 142 px panel width. */
struct WatchFace {
    lv_obj_t * hour = nullptr;
    lv_obj_t * minute = nullptr;
    lv_obj_t * second = nullptr;
};

WatchFace build_watch(lv_obj_t * parent);
void watch_set_time(WatchFace & face, int hour, int minute, int second);
