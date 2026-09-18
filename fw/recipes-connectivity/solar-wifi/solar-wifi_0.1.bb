# WiFi bring-up for the solar-ctl RPi Zero W:
#  - renders /etc/wpa_supplicant/wpa_supplicant-wlan0.conf from build-time
#    variables (set SOLAR_WIFI_SSID/PSK/COUNTRY in local.conf / kas)
#  - two systemd units: wpa_supplicant on wlan0, then busybox udhcpc
SUMMARY = "WiFi bring-up (wpa_supplicant + udhcpc) for solar-ctl"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/COPYING.MIT;md5=3da9cfbcb788c80a0384361b4de20420"

inherit systemd

S = "${UNPACKDIR}"

# Configure from local.conf (or kas local_conf_header). The PSK is baked
# into the image (root-only, 0600), so treat any built image as containing
# your network credentials.
SOLAR_WIFI_SSID ??= "CHANGEME"
SOLAR_WIFI_PSK ??= "CHANGEME"
# ISO 3166-1 alpha-2; sets the WiFi regulatory domain (allowed channels).
SOLAR_WIFI_COUNTRY ??= "GB"

# Export for the do_install shell below (bitbake vars are not in the
# environment by default). Values must not contain '|' characters (sed
# delimiter below).
export WIFI_SSID = "${SOLAR_WIFI_SSID}"
export WIFI_PSK = "${SOLAR_WIFI_PSK}"
export WIFI_COUNTRY = "${SOLAR_WIFI_COUNTRY}"

SRC_URI = " \
    file://solar-wifi.service \
    file://solar-wifi-dhcp.service \
    file://wpa_supplicant-wlan0.conf.template \
"

do_install() {
    install -d ${D}${sysconfdir}/wpa_supplicant
    sed -e "s|@SSID@|${WIFI_SSID}|" \
        -e "s|@PSK@|${WIFI_PSK}|" \
        -e "s|@COUNTRY@|${WIFI_COUNTRY}|" \
        ${UNPACKDIR}/wpa_supplicant-wlan0.conf.template \
        > ${D}${sysconfdir}/wpa_supplicant/wpa_supplicant-wlan0.conf
    chmod 600 ${D}${sysconfdir}/wpa_supplicant/wpa_supplicant-wlan0.conf

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/solar-wifi.service ${D}${systemd_system_unitdir}/
    install -m 0644 ${UNPACKDIR}/solar-wifi-dhcp.service ${D}${systemd_system_unitdir}/
}

SYSTEMD_SERVICE:${PN} = "solar-wifi.service solar-wifi-dhcp.service"
SYSTEMD_AUTO_ENABLE = "enable"

RDEPENDS:${PN} = "wpa-supplicant busybox busybox-udhcpc"
FILES:${PN} += "${sysconfdir}/wpa_supplicant/wpa_supplicant-wlan0.conf"
