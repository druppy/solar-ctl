# inv_ctl - solar-ctl inverter controller

Display/UI half of the inverter controller: **C++23**, **glibmm** main
loop, **LVGL 9.4** scene. Modbus (Deye) traffic and libinput will attach
to the same glib main loop later; today it renders an analog watch as
the display-path proof.

One codebase, two supported builds:

| where | backend | how |
|---|---|---|
| laptop | SDL2 window (142×428, panel scale 1:1) | `cmake -B build && cmake --build build` — fetches LVGL v9.4.0 pinned to the same commit meta-oe builds (needs `cmake`, `libglibmm-2.68-dev`, `libsdl2-dev`) |
| target (Yocto) | Linux framebuffer `/dev/fb0` (NV3007) | `inv-ctl` recipe in `fw/`, links the packaged lvgl (`-DINV_CTL_SYSTEM_LVGL=ON`) |

Both LVGL versions are byte-identical (SRCREV `c016f72d…`), so what you
see in the SDL window is what the panel shows.

## Layout

- `src/main.cpp` — glibmm main loop: ~10 ms LVGL pump, 1 s clock update,
  SIGINT/SIGTERM shutdown. `--backend sdl|fb` (auto: `sdl` when
  `$DISPLAY`/`$WAYLAND_DISPLAY` is set, else `fb`), `--fb DEVICE`.
  The dial follows `Glib::DateTime::create_now_local()`, i.e. the `TZ`
  env var: on the target it is baked from the `SOLAR_TZ` build variable
  into `/etc/default/inv-ctl` (POSIX string — musl has no zoneinfo; see
  `fw/kas-rpi0.yml` `env:`), absent = UTC; on a laptop just run with
  your normal `TZ`.
- `src/backend.cpp` — `lv_sdl_window_create` vs `lv_linux_fbdev_create`
  (compile-gated by `LV_USE_SDL` / `LV_USE_LINUX_FBDEV`). The fb backend
  retries until `/dev/fb0` exists: the panel appears seconds after boot
  once `panel-mipi-dbi` has probed.
- `src/watch.*` — LVGL watch face sized to the 142 px panel width
  (dial, 12 ticks, hour/minute/second hands as pivot-rotated bars).
- `systemd/inv-ctl.service` — installed + enabled by the recipe.
- `native/lv_conf.h` — native-only LVGL config; mirrors the meta-oe
  lvgl defconfig bits that matter (color depth 32, 256 KB pool).

## Target notes

- `/dev/fb0` exists only after `panel-mipi-dbi` probes the NV3007 - and
  the panel stays dark until its init sequence is present as
  `/lib/firmware/panel-mipi-dbi-spi.bin` (vendor code, never fabricated;
  see `fw/README.md` NV3007 section). `inv-ctl.service` therefore retries
  the open forever; check `journalctl -u inv-ctl` for the waiting log.
- The distro pins `PACKAGECONFIG:pn-lvgl = "fbdev"` so the packaged LVGL
  carries the fbdev driver (its default is the DRM one).
