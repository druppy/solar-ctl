# On-target SWUpdate agent glue for the A/B scheme (doc swupdate-ota.md §6):
# signing public key, /etc/hwrevision, the solar-update wrapper, and the
# first-boot kernel seed service. Deliberately NOT a daemon: swupdate runs
# on demand from solar-update (its .service/.socket are stripped from the
# image by solar-ctl-image.bb's postprocess).
SUMMARY = "solar-ctl SWUpdate agent: key, hwrevision, solar-update"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/COPYING.MIT;md5=3da9cfbcb788c80a0384361b4de20420"

SRC_URI = " \
    file://solar-update \
    file://solar-cores-seed \
    file://solar-cores-seed.service \
    file://hwrevision \
"

S = "${UNPACKDIR}"

# do_swu_signing_keys (our class) materializes ${WORKDIR}/swu-signing/
# public.pem from SOLAR_SWU_PUBLIC_KEY (content | path | dev fallback);
# that is what lands in /etc/solar below.
inherit solar-swu-signing
inherit systemd

do_install() {
    install -d ${D}${bindir} ${D}${libexecdir}
    install -d ${D}${sysconfdir}/solar ${D}${sysconfdir}
    install -d ${D}${systemd_system_unitdir}

    install -m 0755 ${S}/solar-update ${D}${bindir}/solar-update
    install -m 0755 ${S}/solar-cores-seed ${D}${libexecdir}/solar-cores-seed
    install -m 0644 ${S}/solar-cores-seed.service ${D}${systemd_system_unitdir}/
    install -m 0644 ${S}/hwrevision ${D}${sysconfdir}/hwrevision
    install -m 0644 ${WORKDIR}/swu-signing/public.pem \
        ${D}${sysconfdir}/solar/swupdate.pub.pem
}

# Enabling: the systemd class ships the unit in FILES and runs the native
# systemctl wrapper at rootfs assembly (works on the RO rootfs).
SYSTEMD_SERVICE:${PN} = "solar-cores-seed.service"

RDEPENDS:${PN} += "swupdate libubootenv-bin"

COMPATIBLE_MACHINE = "^raspberrypi"
