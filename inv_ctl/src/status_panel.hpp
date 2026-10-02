#pragma once

#include "lvgl.hpp"
#include "net.hpp"

/* Right-hand status column (SSID, Wi-Fi signal, IPv4), laid out for the
 * logical landscape view; right-aligned next to the watch dial. */
struct StatusPanel {
    lv_obj_t * ssid = nullptr;
    lv_obj_t * signal = nullptr;
    lv_obj_t * ip = nullptr;
};

StatusPanel build_status_panel(lv_obj_t * parent);
void status_panel_update(StatusPanel & panel, const NetStatus & net);
