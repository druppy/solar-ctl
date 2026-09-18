SUMMARY = "Set kernel RS485 (TIOCSRS485) RTS direction-control on a serial port"
DESCRIPTION = "Tiny helper to enable/disable kernel RS485 mode and tune RTS \
polarity and before/after-send delays, for half-duplex buses with a \
MAX485-style transceiver driven by the UART's RTS line (solar-ctl inverter bus)."
HOMEPAGE = "https://github.com/kloknibor/solar-ctl"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://rs485ctl.c;md5=63e16379edbde0f55b66f767badff5c7"

SRC_URI = "file://rs485ctl.c"

S = "${UNPACKDIR}"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} -o rs485ctl ${UNPACKDIR}/rs485ctl.c
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 rs485ctl ${D}${bindir}/rs485ctl
}
