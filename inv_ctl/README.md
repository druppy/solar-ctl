# inv_ctl - solar-ctl inverter controller

Display/UI half of the inverter controller: **C++23**, **glibmm** main
loop, **LVGL 9.4** scene. Modbus (Deye) traffic and libinput will attach
to the same glib main loop later; today it renders an analog watch as
the display-path proof and shows SWUpdate's OTA progress as an arc page
subscribed to the daemon's progress socket.

One codebase, two supported builds:

| where | backend | how |
|---|---|---|
| laptop | SDL2 window (142×428, panel scale 1:1) | `cmake -B build-ui -DINV_CTL_SWUPDATE_INCLUDE=<swupdate-src>/include && cmake --build build-ui` — LVGL v9.4.0 is fetched, pinned to the same commit meta-oe builds (needs `cmake`, `libglibmm-2.68-dev`, `libsdl2-dev`) |
| target (Yocto) | Linux framebuffer `/dev/fb0` (NV3007) | `inv-ctl` recipe in `fw/`, links the packaged lvgl (`-DINV_CTL_SYSTEM_LVGL=ON`) |

Both LVGL versions are byte-identical (SRCREV `c016f72d…`), so what you
see in the SDL window is what the panel shows.

Dependency policy: **LVGL is the only thing CMake fetches** (embedded
nature, byte-identical pin). Everything else must exist on the platform
or configure fails loudly: swupdate's protocol headers (target:
`swupdate-dev` via the recipe's `DEPENDS`; laptop: `-DINV_CTL_SWUPDATE_INCLUDE=<swupdate-src>/include`,
`export SWUPDATE_SRC=<swupdate-src>`, or apt `libswupdate-dev`) and —
for the unit tests only — Catch2 (apt `libcatch2-dev`). CI pins the
header tree to meta-swupdate's SRCREV `9000ecb5…` (2026.05.1).

## Layout

Everything that renders lives in `src/ui/` (`lvgl.hpp` umbrella,
`backend`, `watch`, `status_panel`, `upgrade`); `src/` holds the app
wiring and the non-LVGL services.

- `src/main.cpp` — glibmm main loop: ~10 ms LVGL pump, 1 s clock update,
  SIGINT/SIGTERM shutdown. `--backend sdl|fb` (auto: `sdl` when
  `$DISPLAY`/`$WAYLAND_DISPLAY` is set, else `fb`), `--fb DEVICE`,
  `--progress-socket PATH` (default: resolved like the daemon).
  SIGTERM (systemd shutdown, the OTA reboot lands here) paints
  "Restarting" and flushes it to the panel before quitting; SIGINT
  (dev Ctrl-C) exits plainly.
  The dial follows `Glib::DateTime::create_now_local()`, i.e. the `TZ`
  env var: on the target it is baked from the `SOLAR_TZ` build variable
  into `/etc/default/inv-ctl` (POSIX string — musl has no zoneinfo; see
  `fw/kas-rpi0.yml` `env:`), absent = UTC; on a laptop just run with
  your normal `TZ`.
- `src/ui/backend.cpp` — `lv_sdl_window_create` vs `lv_linux_fbdev_create`
  (compile-gated by `LV_USE_SDL` / `LV_USE_LINUX_FBDEV`). The fb backend
  retries until `/dev/fb0` exists: the panel appears seconds after boot
  once `panel-mipi-dbi` has probed.
- `src/ui/watch.*` — LVGL watch face sized to the 142 px panel width
  (dial, 12 ticks, hour/minute/second hands as pivot-rotated bars).
- `src/ui/status_panel.*` — the status text block under the watch: WiFi
  link/signal (fed by `src/net.*`) and the board lines (fed by
  `src/board.*`).
- `src/board.*` — Raspberry Pi model + SoC serial from the device tree
  (`/sys/firmware/devicetree/base/{model,serial-number}`, read once at
  startup); the status panel shows them as two extra lines. On machines
  without a Pi DT (x86 laptop on ACPI) `board_query()` returns empty and
  the labels are not created.
- `src/update.*` — the SWUpdate progress IPC client. ALL protocol
  definitions (structs, `RECOVERY_STATUS`/`sourcetype`, API version,
  ACK magic) come from swupdate's own headers, never from copies:
  `swupdate-dev` in the sysroot on the target (`DEPENDS += swupdate`),
  on laptop/CI located via `-DINV_CTL_SWUPDATE_INCLUDE` or the system
  `libswupdate-dev` (see the dependency policy above).
  On the target even the socket-path resolution IS upstream's
  `get_prog_socket()`, linked from libswupdate (which is how the
  `swupdate-ipc` package lands in the image); native keeps a small
  fallback mirror. Parsing yields the raw upstream frames; print/display
  sites read the char arrays through `bounded_str()`. Frames: 8-byte
  connect ACK ("ACK\0" + API major gate), then fixed-size `progress_msg`
  LE frames — same layout swupdate's own clients use.
  `tests/test_update.cpp` covers framing gates, field-bound paranoia,
  status→action policy, percent selection and the socket-path fallback.
- `src/updater.*` — glibmm listener (`Glib::signal_io`) on that socket:
  consumes the ACK, then whole frames; every message — including
  everything that is not progress (info payloads, DONE, unknown
  statuses) — is logged via `g_message` to journald. The daemon supports
  any number of progress clients (`core/progress_thread.c` broadcasts to
  a list), so this coexists with `solar-swupdate-progress.service`,
  which stays the sole reboiler. Idle = silence, and reconnects are a
  quiet 5 s retry loop.
- `src/ui/upgrade.*` — the OTA page: a second screen with a default-theme
  arc as a read-only gauge (knob hidden) and one label —
  `Upgrading / <protocol state> / <percent>%`, where the percent is
  `dwl_percent` while streaming and `cur_percent` per install step.
  SUCCESS → centered "Restarting"; FAILURE → red "Upgrade failed" for
  10 s, then the watch back (no reboot follows a failed update).
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
