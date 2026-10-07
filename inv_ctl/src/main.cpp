/* inv-ctl: solar-ctl inverter controller.
 *
 * Current scope: render an analog watch plus a Wi-Fi/IP status column as
 * proof of the display path - SDL2 on a laptop, Linux framebuffer (NV3007
 * panel) on the target, selected by --backend or auto-detected from the
 * session. The glibmm main loop also hosts the ~10 ms LVGL pump, the 1 s
 * clock/network update, the swupdate progress listener (upgrade page)
 * and signal handling; the libinput and Modbus event sources will be
 * attached to this same loop later. */
#include "ui/backend.hpp"
#include "ui/lvgl.hpp"
#include "ui/status_panel.hpp"
#include "ui/upgrade.hpp"
#include "ui/watch.hpp"
#include "board.hpp"
#include "net.hpp"
#include "update.hpp"
#include "updater.hpp"

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

constexpr uint32_t kColorText = 0xe6edf3; /* dial off-white */
constexpr uint32_t kColorFail = 0xef4444; /* dial second-hand red */
constexpr int kFailBackMs = 10000;        /* "Upgrade failed" linger time */

struct QuitCtx {
    Glib::MainLoop * loop = nullptr;
    UpgradeScreen * upg = nullptr;
    bool restarting = false; /* SIGTERM (systemd shutdown) vs SIGINT (dev Ctrl-C) */
};

gboolean quit_main_loop(gpointer user_data)
{
    const auto * ctx = static_cast<const QuitCtx *>(user_data);
    if (ctx->restarting) {
        /* systemd's shutdown follows immediately; put the last frame on the
         * panel first (the fbdev flush runs inside lv_timer_handler). */
        upgrade_message(*ctx->upg, "Restarting", kColorText);
        lv_screen_load(ctx->upg->screen);
        for (int i = 0; i < 20; ++i) {
            lv_timer_handler();
            Glib::usleep(10 * G_USEC_PER_SEC);
        }
    }
    ctx->loop->quit();
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
    Glib::ustring progress_socket; /* empty: resolve like the daemon does */

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

    Glib::OptionEntry prog_entry;
    prog_entry.set_long_name("progress-socket");
    prog_entry.set_description("swupdate progress socket (default: resolved like the daemon)");
    prog_entry.set_arg_description("PATH");
    group.add_entry(prog_entry, progress_socket);

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
    const BoardInfo board = board_query();
    StatusPanel panel = build_status_panel(lv_screen_active(), board);

    const string_view netif = net_if.c_str();   /* outlives the loop: both are main-scope */
    NetStatus net{};
    auto update_all = [&face, &panel, &net, netif] {
        const auto now = Glib::DateTime::create_now_local();
        watch_set_time(face, now.get_hour(), now.get_minute(), now.get_second());
        net_refresh(net, netif);
        status_panel_update(panel, net);
    };
    update_all();

    /* OTA page: a second screen (not loaded) plus the swupdate progress
     * subscription. The reboot itself belongs to solar-swupdate-progress ->
     * solar-swu-reboot; this is display-only. */
    UpgradeScreen upgrade = build_upgrade_screen();
    lv_obj_t * const main_screen = lv_screen_active();

    Updater updater(progress_socket.empty() ? prog_socket_path() : string(progress_socket));
    /* Generation counter supersedes pending "hand the screen back" timers:
     * cancelling a connect_once needs a connection handle, and that handle
     * type is glibmm-generation dependent (2.66 target vs 2.90 host). */
    unsigned ota_gen = 0;
    /* Monotonic composition of the daemon's two percent axes (see
     * overall_percent); reset when a new install announces itself. */
    unsigned overall = 0;
    updater.start([&upgrade, main_screen, &ota_gen, &overall](const progress_msg & m) {
        switch (action_for(m)) {
        case Action::Ignore:
            break; /* logged by the Updater (info payloads included) */
        case Action::Upgrade: {
            ++ota_gen;
            if (m.status == START)
                overall = 0;
            overall = overall_percent(m, overall);
            if (lv_screen_active() != upgrade.screen)
                lv_screen_load(upgrade.screen);
            /* Raw state token plus the step counter, so the alternating
             * DOWNLOAD/PROGRESS messages read as one story on the fixed-
             * width label (the arc shows the composed percent). */
            string state(state_name(m.status));
            if (m.cur_step > 0 && m.nsteps > 0) {
                state += " ";
                state += to_string(m.cur_step);
                state += '/';
                state += to_string(m.nsteps);
            }
            upgrade_status(upgrade, state, overall);
            break;
        }
        case Action::Succeeded:
            ++ota_gen;
            lv_screen_load(upgrade.screen);
            upgrade_message(upgrade, "Restarting", kColorText);
            break;
        case Action::Failed: {
            /* No reboot follows a failed update: stand out in red, then
             * hand the screen back - unless a newer OTA superseded it. */
            const unsigned gen = ++ota_gen;
            lv_screen_load(upgrade.screen);
            upgrade_message(upgrade, "Upgrade failed", kColorFail);
            Glib::signal_timeout().connect_once(
                [&upgrade, main_screen, &ota_gen, gen] {
                    if (ota_gen == gen && lv_screen_active() == upgrade.screen)
                        lv_screen_load(main_screen);
                },
                kFailBackMs);
            break;
        }
        }
    });

    /* LVGL pump. */
    Glib::signal_timeout().connect([] { lv_timer_handler(); return true; }, 10);
    /* 1 s clock + network tick (future: same loop drives libinput + Modbus). */
    Glib::signal_timeout().connect([update_all] { update_all(); return true; }, 1000);

    /* glibmm 2.88 has no Glib::signal_unix_signal(); use GLib's own
     * g_unix_signal_add() so SIGTERM/SIGINT end the loop cleanly. SIGTERM
     * is systemd's stop signal (the OTA reboot lands here): put "Restarting"
     * on the panel before quitting. SIGINT (dev Ctrl-C) exits plainly. */
    QuitCtx int_ctx{loop.get(), &upgrade, false};
    QuitCtx term_ctx{loop.get(), &upgrade, true};
    g_unix_signal_add(SIGINT, &quit_main_loop, &int_ctx);
    g_unix_signal_add(SIGTERM, &quit_main_loop, &term_ctx);

    loop->run();
    return 0;
}
