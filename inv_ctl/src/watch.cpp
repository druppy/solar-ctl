#include "watch.hpp"

#include <cmath>

namespace {

constexpr int kDial = 130;               /* fits the 142 px panel width */
constexpr int kCenter = kDial / 2;

/* A hand: thin bar whose bottom edge sits on the dial centre and which
 * rotates around that pivot. Rotation unit is 0.1 degrees, 0 = 12
 * o'clock, positive = clockwise. */
lv_obj_t * make_hand(lv_obj_t * parent, int width, int length, uint32_t color)
{
    lv_obj_t * hand = lv_obj_create(parent);
    lv_obj_remove_style_all(hand);
    lv_obj_set_size(hand, width, length);
    lv_obj_set_pos(hand, kCenter - width / 2, kCenter - length);
    lv_obj_set_style_transform_pivot_x(hand, width / 2, 0);
    lv_obj_set_style_transform_pivot_y(hand, length, 0);
    lv_obj_set_style_bg_color(hand, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(hand, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hand, width / 2, 0);
    return hand;
}

void make_ticks(lv_obj_t * dial)
{
    constexpr double pi = 3.14159265358979323846;
    for (int i = 0; i < 12; ++i) {
        const double angle = i * (pi / 6.0);
        constexpr double r = kCenter - 9.0;
        lv_obj_t * tick = lv_obj_create(dial);
        lv_obj_remove_style_all(tick);
        /* The 12 marker is one pixel wider than the other quarter ticks:
         * glanceable proof that the dial shares the text orientation. */
        const int w = (i == 0) ? 4 : 3;
        lv_obj_set_size(tick, w, (i % 3 == 0) ? 10 : 6);
        const int x = static_cast<int>(kCenter + r * std::sin(angle) - w / 2.0);
        const int y = static_cast<int>(kCenter - r * std::cos(angle) - 5);
        lv_obj_set_pos(tick, x, y);
        lv_obj_set_style_bg_color(tick, lv_color_hex(0x8b98a5), 0);
        lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
        lv_obj_set_style_transform_rotation(tick, i * 30 * 10, 0);
    }
}

void set_angle(lv_obj_t * hand, double deg)
{
    lv_obj_set_style_transform_rotation(hand, static_cast<int>(deg * 10), 0);
}

} // namespace

WatchFace build_watch(lv_obj_t * parent)
{
    lv_obj_t * dial = lv_obj_create(parent);
    lv_obj_remove_style_all(dial);
    lv_obj_set_size(dial, kDial, kDial);
    /* Logical landscape: dial on the left, status panel on the right. */
    lv_obj_align(dial, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_bg_color(dial, lv_color_hex(0x0e1116), 0);
    lv_obj_set_style_bg_opa(dial, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dial, 2, 0);
    lv_obj_set_style_border_color(dial, lv_color_hex(0x2e6b4f), 0);
    lv_obj_set_style_radius(dial, LV_RADIUS_CIRCLE, 0);

    make_ticks(dial);

    WatchFace face;
    face.hour = make_hand(dial, 5, 38, 0xe6edf3);
    face.minute = make_hand(dial, 3, 52, 0xc9d5e1);
    face.second = make_hand(dial, 2, 57, 0xef4444);

    lv_obj_t * cap = lv_obj_create(dial);
    lv_obj_remove_style_all(cap);
    lv_obj_set_size(cap, 8, 8);
    lv_obj_center(cap);
    lv_obj_set_style_bg_color(cap, lv_color_hex(0xe6edf3), 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);

    watch_set_time(face, 10, 10, 30); /* neutral pose before the first tick */
    return face;
}

void watch_set_time(WatchFace & face, int hour, int minute, int second)
{
    set_angle(face.second, second * 6.0);
    set_angle(face.minute, minute * 6.0 + second * 0.1);
    set_angle(face.hour, (hour % 12) * 30.0 + minute * 0.5);
}
