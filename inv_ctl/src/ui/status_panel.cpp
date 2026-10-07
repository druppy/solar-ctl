#include "status_panel.hpp"

#include <cstdio>

using namespace std;

StatusPanel build_status_panel(lv_obj_t * parent, const BoardInfo & board)
{
    lv_obj_t * col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_width(col, LV_SIZE_CONTENT);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_align(col, LV_ALIGN_RIGHT_MID, -10, 0);

    StatusPanel panel;
    panel.ssid = lv_label_create(col);
    panel.signal = lv_label_create(col);
    panel.ip = lv_label_create(col);
    lv_obj_set_width(panel.ssid, LV_SIZE_CONTENT);
    lv_label_set_text(panel.ssid, "SSID: --");
    lv_label_set_text(panel.signal, "no link");
    lv_label_set_text(panel.ip, "IP: --");

    /* Static board identity; empty BoardInfo (laptop, non-Pi board) keeps
     * the column exactly as it was. */
    if (!board.model.empty()) {
        char buf[64];
        panel.model = lv_label_create(col);
        lv_label_set_text(panel.model, board.model.c_str());
        snprintf(buf, sizeof(buf), "SN: %s", board.serial.c_str());
        panel.serial = lv_label_create(col);
        lv_label_set_text(panel.serial, buf);
    }
    return panel;
}

void status_panel_update(StatusPanel & panel, const NetStatus & net)
{
    char buf[64];

    snprintf(buf, sizeof(buf), "SSID: %s", net.ssid.c_str());
    lv_label_set_text(panel.ssid, buf);

    if (net.linked)
        snprintf(buf, sizeof(buf), "WiFi: %d dBm", net.signal_dbm);
    else
        snprintf(buf, sizeof(buf), "WiFi: no link");
    lv_label_set_text(panel.signal, buf);

    snprintf(buf, sizeof(buf), "IP: %s", net.ipv4.c_str());
    lv_label_set_text(panel.ip, buf);
}
