#pragma once

#include "lvgl.hpp"

#include <string>

/* Creates the display backend, or nullptr + err message on failure
 * (e.g. the framebuffer does not exist yet - the NV3007 panel appears
 * some seconds after boot once panel-mipi-dbi has probed).
 *
 * rotation_deg (0/90/180/270) is the panel's physical mounting rotation:
 * the UI is always designed in the resulting *logical* space (90 => the
 * landscape 428x142 view of the portrait panel). On fb it is applied via
 * lv_display_set_rotation() (LVGL swaps the logical resolution and
 * rotates the rendered frame on flush); the SDL window is created at the
 * logical size directly, so the desktop preview is upright. */
lv_display_t * create_display(const std::string & backend,
                              const std::string & fbdev,
                              int rotation_deg,
                              std::string & err);
