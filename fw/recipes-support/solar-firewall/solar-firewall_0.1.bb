# solar-ctl nftables firewall loader (2026-10-06): default-deny IPv4 input,
# allow loopback, replies, ICMP echo, SSH (22) and the SWUpdate HTTP server
# (8080). Kernel side (NF_TABLES/CONNTRACK/NFT_CT) is built-in via
# solar-ctl-slim.cfg, so the ruleset loads with zero module loading; we
# deliberately ship no kernel-module-* packages here (.rules policy).
# The nftables recipe lives in meta-networking (added in fw/kas-rpi0.yml);
# PACKAGECONFIG there is trimmed to mini-gmp in the distro conf (no
# python/json/readline runtime fat).
SUMMARY = "solar-ctl nftables firewall (default-deny, SSH + SWUpdate open)"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/COPYING.MIT;md5=3da9cfbcb788c80a0384361b4de20420"

SRC_URI = " \
    file://solar.nft \
    file://solar-firewall.service \
"

S = "${UNPACKDIR}"

inherit systemd

do_install() {
    install -d ${D}${sysconfdir}/nftables
    install -m 0644 ${S}/solar.nft ${D}${sysconfdir}/nftables/solar.nft

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${S}/solar-firewall.service \
        ${D}${systemd_system_unitdir}/solar-firewall.service
}

SYSTEMD_SERVICE:${PN} = "solar-firewall.service"

RDEPENDS:${PN} += "nftables"

COMPATIBLE_MACHINE = "^raspberrypi"
