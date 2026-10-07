#pragma once

#include "lvgl.hpp"

#include <cstdint>
#include <string_view>

/* Full-screen OTA page shown on top of the watch screen while the swupdate
 * daemon reports activity (lv_screen_load switches; the watch keeps living
 * on its own screen underneath). build_upgrade_screen() creates it but does
 * NOT make it active.
 *
 * Deliberately plain: default-theme arc as a read-only gauge (knob hidden,
 * not clickable), one label - no bespoke colors except the red the user
 * asked for on FAILURE. */
struct UpgradeScreen {
    lv_obj_t * screen = nullptr;
    lv_obj_t * arc = nullptr;
    lv_obj_t * label = nullptr;
};

UpgradeScreen build_upgrade_screen();

/* Three lines: "Upgrading" / raw protocol state / percent, arc = percent. */
void upgrade_status(UpgradeScreen & us, std::string_view state, unsigned percent);

/* Single centered line, arc hidden (used for "Upgrade failed" in red and
 * for "Restarting"). */
void upgrade_message(UpgradeScreen & us, std::string_view text, std::uint32_t rgb);
