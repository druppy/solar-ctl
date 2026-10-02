SUMMARY = "NV3007 display-controller init firmware for the kernel panel-mipi-dbi driver"
DESCRIPTION = "Binary command list consumed by the mainline panel-mipi-dbi driver via \
request_firmware(); the file name must be the DT compatible plus '.bin' \
(see drivers/gpu/drm/tiny/panel-mipi-dbi.c). Generated from the LVGL v9.4.0 in-tree \
NV3007 driver (itself based on Arduino_GFX) by fw/tools/lvgl-nv3007-to-mipi-dbi.py \
-- regenerate and review the --dump output when bumping LVGL; files/provenance.txt \
carries the exact command line and MIT attribution."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://provenance.txt;md5=9fed1554af82d834afa0a5c8f91ac7c5"

SRC_URI = "file://panel-mipi-dbi-spi.bin \
           file://provenance.txt \
           "

S = "${UNPACKDIR}"

do_install() {
    install -d ${D}${nonarch_base_libdir}/firmware
    install -m 0644 ${UNPACKDIR}/panel-mipi-dbi-spi.bin ${D}${nonarch_base_libdir}/firmware/
}

FILES:${PN} = "${nonarch_base_libdir}/firmware/panel-mipi-dbi-spi.bin"
