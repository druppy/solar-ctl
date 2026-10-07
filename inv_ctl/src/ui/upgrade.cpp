/* LVGL side of the OTA page; policy (which text, which screen) lives in
 * main.cpp, this file only draws what it is told. */
#include "upgrade.hpp"

#include <cstdio>

using namespace std;

namespace {

constexpr int kArc = 116; /* leaves room for the theme's arc line inside 142 px */
constexpr uint32_t kText = 0xe6edf3;   /* the dial's off-white (watch.cpp)     */
constexpr uint32_t kBg = 0x0e1116;     /* the dial's background                */

} // namespace

UpgradeScreen build_upgrade_screen()
{
    /* A screen with an explicit plain background: lv_obj_create(NULL) gets
     * a generic themed object, whose card styles (radius, padding) would
     * otherwise show through. */
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kBg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    UpgradeScreen us;
    us.screen = scr;

    us.arc = lv_arc_create(scr);
    lv_obj_set_size(us.arc, kArc, kArc);
    lv_arc_set_range(us.arc, 0, 100);
    lv_arc_set_value(us.arc, 0);
    lv_obj_remove_flag(us.arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(us.arc, LV_OPA_TRANSP, LV_PART_KNOB); /* gauge, not control */
    lv_obj_align(us.arc, LV_ALIGN_LEFT_MID, 6, 0); /* mirrors the watch dial */

    us.label = lv_label_create(scr);
    lv_obj_set_style_text_color(us.label, lv_color_hex(kText), 0);
    lv_obj_align(us.label, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_label_set_text(us.label, "Upgrading\nIDLE\n0%");

    return us;
}

void upgrade_status(UpgradeScreen & us, string_view state, unsigned percent)
{
    lv_obj_remove_flag(us.arc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(us.label, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_text_color(us.label, lv_color_hex(kText), 0);
    lv_arc_set_value(us.arc, static_cast<int>(percent));
    lv_label_set_text_fmt(us.label, "Upgrading\n%.*s\n%u%%",
                          static_cast<int>(state.size()), state.data(), percent);
}

void upgrade_message(UpgradeScreen & us, string_view text, uint32_t rgb)
{
    lv_obj_add_flag(us.arc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(us.label);
    lv_obj_set_style_text_color(us.label, lv_color_hex(rgb), 0);
    lv_label_set_text_fmt(us.label, "%.*s", static_cast<int>(text.size()), text.data());
}
