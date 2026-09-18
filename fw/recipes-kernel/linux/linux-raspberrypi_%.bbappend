# solar-ctl kernel customisation for linux-raspberrypi.
#
# 1. solar-ctl-slim.cfg drops the desktop-Pi display/media/sound stack
#    that bcmrpi_defconfig enables (kernel-yocto auto-applies *.cfg
#    files from SRC_URI).
# 2. The nv3007 / solar-rs485 device-tree overlays are copied into
#    arch/arm/boot/dts/overlays/ and registered in that directory's
#    Makefile (patch): kbuild only builds overlays listed in its
#    dtbo-y, so an unregistered .dts has "no rule" even when passed
#    as an explicit `make overlays/x.dtbo` target (verified on this
#    tree).
#    The matching RPI_KERNEL_DEVICETREE_OVERLAYS:append (which also
#    puts the dtbos on the boot partition) lives in the kas
#    local_conf_header so that the image recipe sees it too -
#    IMAGE_BOOT_FILES is generated there, not from this recipe.

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += " \
    file://solar-ctl-slim.cfg \
    file://nv3007-overlay.dts \
    file://solar-rs485-overlay.dts \
    file://0001-overlays-register-nv3007-and-solar-rs485.patch \
"

do_configure:append() {
    install -m 0644 ${UNPACKDIR}/nv3007-overlay.dts \
        ${S}/arch/arm/boot/dts/overlays/nv3007-overlay.dts
    install -m 0644 ${UNPACKDIR}/solar-rs485-overlay.dts \
        ${S}/arch/arm/boot/dts/overlays/solar-rs485-overlay.dts
}
