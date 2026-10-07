# Inverter controller UI (repo-root inv_ctl/, delivered via file:// - the
# solar-ctl repo deliberately has no git URL in kas, see .rules).
#
# LVGL comes from meta-oe (PACKAGECONFIG:pn-lvgl = "fbdev" in the distro
# conf gives us the LV_USE_LINUX_FBDEV driver); glibmm-2.68 is the newest
# glibmm (2.88, glib-2.68 ABI series).
#
# swupdate supplies the progress-IPC protocol definitions (its -dev
# package installs /usr/include/progress_ipc.h + swupdate_status.h) and
# libswupdate, which we link for get_prog_socket(). The shlib auto-dep
# pulls the swupdate-ipc package (libswupdate0.1, ~18 KB) into the image
# - the daemon binary itself embeds these objects statically, so this is
# a genuinely new runtime package, not one already present.

SUMMARY = "solar-ctl inverter controller (LVGL UI)"
DESCRIPTION = "Display/UI half of the solar-ctl inverter controller: LVGL scene on the Linux framebuffer (NV3007 2.79-inch panel), glibmm main loop. Modbus transport attaches to the same loop later."
HOMEPAGE = "https://github.com/druppy/solar-ctl"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=1d87715cd026dc21bccddaf6ecb9b94d"

# Repo root is three levels up from this recipe dir
# (fw/recipes-apps/inv-ctl). THISDIR is defined at recipe-parse time, so
# abspath() it inline (LAYERDIR is a layer.conf variable - in a .bb it
# would stay literal inside FILESEXTRAPATHS and never be found).
FILESEXTRAPATHS:prepend := "${@os.path.abspath('${THISDIR}/../../..')}:"
SRC_URI = "file://inv_ctl"
S = "${UNPACKDIR}/inv_ctl"

DEPENDS = "lvgl glibmm-2.68 swupdate"

inherit cmake pkgconfig systemd

EXTRA_OECMAKE = "-DINV_CTL_SYSTEM_LVGL=ON -DINV_CTL_SYSTEM_SWUPDATE=ON"

SYSTEMD_SERVICE:${PN} = "inv-ctl.service"
SYSTEMD_AUTO_ENABLE = "enable"

# Watch timezone (fw/kas-rpi0.yml env:). Empty = no TZ baked, the watch
# shows UTC. Must be a POSIX TZ string: musl's localtime() has no
# zoneinfo support, so 'Europe/Copenhagen' is silently ignored and the
# clock stays UTC - a leading colon (glibc file form) is likewise dead.
SOLAR_TZ ??= ""
do_install[vardeps] += "SOLAR_TZ"

do_install:append() {
    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${S}/systemd/inv-ctl.service ${D}${systemd_unitdir}/system/

    if [ -n "${SOLAR_TZ}" ]; then
        # Reject zoneinfo names: musl's TZ parser has no zoneinfo, so
        # 'Europe/Copenhagen' is silently ignored (clock stays UTC) -
        # worse than a build error. A POSIX spec always carries digits
        # (the offset), a zoneinfo name never does; their '/' is only
        # legal as a 'M10.5.0/3' transition-time suffix. A leading ':'
        # is glibc's file form, equally dead on musl.
        case "${SOLAR_TZ}" in
            :*)
                bbfatal "SOLAR_TZ must not use glibc's ':zoneinfo' file form - musl has no zoneinfo (clock would stay UTC)."
                ;;
            *[0-9]*) :
                # POSIX spec - unless it is one of the digit-carrying
                # zoneinfo names (Etc/GMT+5), which musl also ignores.
                case "${SOLAR_TZ}" in
                    Etc/*)
                        bbfatal "SOLAR_TZ='${SOLAR_TZ}' is a zoneinfo name (Etc/*): musl has no zoneinfo; write the bare POSIX name instead (e.g. GMT5 for Etc/GMT+5)."
                        ;;
                esac
                ;;
            */*)
                bbfatal "SOLAR_TZ looks like a zoneinfo name: musl has no zoneinfo, it would be silently ignored and the clock stay UTC. Use a POSIX TZ string, e.g. CET-1CEST,M3.5.0,M10.5.0/3 (Copenhagen) or GMT0BST,M3.5.0/1,M10.5.0/1 (London)."
                ;;
        esac
        install -d ${D}${sysconfdir}/default
        printf 'TZ=%s\n' "${SOLAR_TZ}" > ${D}${sysconfdir}/default/inv-ctl
        chmod 0644 ${D}${sysconfdir}/default/inv-ctl
        bbnote "inv-ctl: baking TZ=${SOLAR_TZ} into /etc/default/inv-ctl"
    fi
}
