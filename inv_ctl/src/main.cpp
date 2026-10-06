/* inv-ctl: solar-ctl inverter controller.
 *
 * Current scope: render an analog watch plus a Wi-Fi/IP status column as
 * proof of the display path - SDL2 on a laptop, Linux framebuffer (NV3007
 * panel) on the target, selected by --backend or auto-detected from the
 * session. The glibmm main loop also hosts the ~10 ms LVGL pump, the 1 s
 * clock/network update and signal handling; the libinput and Modbus event
 * sources will be attached to this same loop later. */
#include "backend.hpp"
#include "lvgl.hpp"
#include "net.hpp"
#include "status_panel.hpp"
#include "watch.hpp"

#include <glib.h>
#include <glib-unix.h>
#include <glibmm.h>

#include <csignal>
#include <cstdio>
#include <string>
#include <string_view>

using namespace std;

namespace {

uint32_t lv_tick_from_glib()
{
    return static_cast<uint32_t>(g_get_monotonic_time() / 1000);
}

gboolean quit_main_loop(gpointer user_data)
{
    static_cast<Glib::MainLoop *>(user_data)->quit();
    return G_SOURCE_CONTINUE;
}

} // namespace

int main(int argc, char * argv[])
{
    Glib::init();

    Glib::OptionContext context("Render the solar-ctl controller UI.");
    Glib::OptionGroup group("display", "display backend options");
    Glib::ustring backend = "auto";
    Glib::ustring fbdev = "/dev/fb0";
    /* Bench-verified viewing orientation: the UI is designed logical
     * landscape; at 90 the status text and dial are upright as viewed on
     * the bench (270 would be the view from the opposite side). */
    Glib::ustring rotate = "90";
    Glib::ustring net_if = "wlan0";

    Glib::OptionEntry backend_entry;
    backend_entry.set_long_name("backend");
    backend_entry.set_description("drawing backend (auto: sdl inside a desktop session, else fb)");
    backend_entry.set_arg_description("sdl|fb");
    group.add_entry(backend_entry, backend);

    Glib::OptionEntry fb_entry;
    fb_entry.set_long_name("fb");
    fb_entry.set_description("framebuffer device for the fb backend");
    fb_entry.set_arg_description("DEVICE");
    group.add_entry(fb_entry, fbdev);

    Glib::OptionEntry rotate_entry;
    rotate_entry.set_long_name("rotate");
    rotate_entry.set_description("panel mounting rotation (applied on the fb backend)");
    rotate_entry.set_arg_description("0|90|180|270");
    group.add_entry(rotate_entry, rotate);

    Glib::OptionEntry net_entry;
    net_entry.set_long_name("net-if");
    net_entry.set_description("interface shown in the status panel");
    net_entry.set_arg_description("NAME");
    group.add_entry(net_entry, net_if);

    context.set_main_group(group);
    try {
        context.parse(argc, argv);
    } catch (const exception & e) {
        fprintf(stderr, "inv-ctl: %s\n", e.what());
        return 2;
    }

    if (backend == "auto")
        backend = (g_getenv("DISPLAY") || g_getenv("WAYLAND_DISPLAY")) ? "sdl" : "fb";

    int rotation = 90;
    try {
        rotation = stoi(rotate);
    } catch (...) {
        rotation = -1;
    }
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) {
        fprintf(stderr, "inv-ctl: --rotate must be 0, 90, 180 or 270\n");
        return 2;
    }

    lv_init();
    lv_tick_set_cb(&lv_tick_from_glib);

    /* The framebuffer appears only once panel-mipi-dbi has probed the
     * panel (and only at all once the NV3007 init firmware is present),
     * so retry until the backend comes up. */
    auto loop = Glib::MainLoop::create();
    lv_display_t * disp = nullptr;
    for (unsigned attempt = 1; !disp; ++attempt) {
        string err;
        // ustring -> string_view via its NUL-terminated buffer (alive here).
        disp = create_display(backend.c_str(), fbdev.c_str(), rotation, err);
        if (!disp) {
            if (attempt == 1 || attempt % 15 == 0)
                g_message("inv-ctl: waiting for display (%s): %s", backend.c_str(), err.c_str());
            Glib::usleep(2 * G_USEC_PER_SEC);
        }
    }

    /* Dark theme for everything that does not carry explicit styles. */
    lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE),
                          lv_palette_main(LV_PALETTE_GREEN), true, LV_FONT_DEFAULT);

    WatchFace face = build_watch(lv_screen_active());
    StatusPanel panel = build_status_panel(lv_screen_active());

    const string_view netif = net_if.c_str();   /* outlives the loop: both are main-scope */
    NetStatus net{};
    auto update_all = [&face, &panel, &net, netif] {
        const auto now = Glib::DateTime::create_now_local();
        watch_set_time(face, now.get_hour(), now.get_minute(), now.get_second());
        net_refresh(net, netif);
        status_panel_update(panel, net);
    };
    update_all();

    /* LVGL pump. */
    Glib::signal_timeout().connect([] { lv_timer_handler(); return true; }, 10);
    /* 1 s clock + network tick (future: same loop drives libinput + Modbus). */
    Glib::signal_timeout().connect([update_all] { update_all(); return true; }, 1000);

    /* glibmm 2.88 has no Glib::signal_unix_signal(); use GLib's own
     * g_unix_signal_add() so SIGTERM/SIGINT end the loop cleanly. */
    g_unix_signal_add(SIGINT, &quit_main_loop, loop.get());
    g_unix_signal_add(SIGTERM, &quit_main_loop, loop.get());

    loop->run();
    return 0;
}
