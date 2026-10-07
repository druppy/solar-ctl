#include "backend.hpp"

#include <cerrno>
#include <cstring>

#include <unistd.h>

using namespace std;

/* Panel geometry: TZT 2.79" NV3007 module, native portrait. */
constexpr int kPanelHor = 142;
constexpr int kPanelVer = 428;

lv_display_t * create_display(string_view backend,
                              string_view fbdev,
                              int rotation_deg,
                              string & err)
{
    const bool swap = (rotation_deg % 180) != 0;

    if (backend == "fb") {
#if LV_USE_LINUX_FBDEV
        /* C boundary (access/LVGL): one null-terminated copy for both. */
        const string fb{fbdev};
        /* Pre-check: the panel (and thus /dev/fb0) appears only after
         * panel-mipi-dbi has probed; caller retries. */
        if (access(fb.c_str(), W_OK) != 0) {
            err = fb + ": " + strerror(errno);
            return nullptr;
        }
        lv_display_t * disp = lv_linux_fbdev_create();
        if (!disp) {
            err = "lv_linux_fbdev_create failed";
            return nullptr;
        }
        lv_linux_fbdev_set_file(disp, fb.c_str());
        const lv_display_rotation_t rot =
            rotation_deg == 90  ? LV_DISPLAY_ROTATION_90 :
            rotation_deg == 180 ? LV_DISPLAY_ROTATION_180 :
            rotation_deg == 270 ? LV_DISPLAY_ROTATION_270
                                : LV_DISPLAY_ROTATION_0;
        lv_display_set_rotation(disp, rot);
        return disp;
#else
        err = "built without framebuffer support";
        return nullptr;
#endif
    }

    if (backend == "sdl") {
#if LV_USE_SDL
        /* Desktop preview: window at the logical (post-rotation) size,
         * content upright - no display rotation applied here. */
        lv_display_t * disp = lv_sdl_window_create(swap ? kPanelVer : kPanelHor,
                                                   swap ? kPanelHor : kPanelVer);
        if (!disp) {
            err = "lv_sdl_window_create failed";
            return nullptr;
        }
        lv_sdl_window_set_title(disp, "inv-ctl");
        return disp;
#else
        err = "built without SDL support";
        return nullptr;
#endif
    }

    err = "unknown backend '" + string(backend) + "' (use sdl or fb)";
    return nullptr;
}
