#pragma once

#include "board.hpp"
#include "lvgl.hpp"
#include "net.hpp"

/* Right-hand status column (SSID, Wi-Fi signal, IPv4, plus the Raspberry Pi
 * model + serial when running on one), laid out for the logical landscape
 * view; right-aligned next to the watch dial. */
struct StatusPanel {
    lv_obj_t * ssid = nullptr;
    lv_obj_t * signal = nullptr;
    lv_obj_t * ip = nullptr;
    lv_obj_t * model = nullptr;   /* nullptr = not a Raspberry Pi */
    lv_obj_t * serial = nullptr;
};

StatusPanel build_status_panel(lv_obj_t * parent, const BoardInfo & board);
void status_panel_update(StatusPanel & panel, const NetStatus & net);
