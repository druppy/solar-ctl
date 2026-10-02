# Inverter controller UI (repo-root inv_ctl/, delivered via file:// - the
# solar-ctl repo deliberately has no git URL in kas, see .rules).
#
# LVGL comes from meta-oe (PACKAGECONFIG:pn-lvgl = "fbdev" in the distro
# conf gives us the LV_USE_LINUX_FBDEV driver); glibmm-2.68 is the newest
# glibmm (2.88, glib-2.68 ABI series).

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

DEPENDS = "lvgl glibmm-2.68"

inherit cmake pkgconfig systemd

EXTRA_OECMAKE = "-DINV_CTL_SYSTEM_LVGL=ON"

SYSTEMD_SERVICE:${PN} = "inv-ctl.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install:append() {
    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${S}/systemd/inv-ctl.service ${D}${systemd_unitdir}/system/
}
