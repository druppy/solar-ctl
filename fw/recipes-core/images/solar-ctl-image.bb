# Minimal firmware image for solar-ctl.
#
# core-image-base already drags in packagegroup-core-basic, which pulls
# util-linux, shadow and other fat we don't want. Start from the bare
# -minimal recipe instead and add back only the essentials.
require recipes-core/images/core-image-minimal.bb

SUMMARY = "solar-ctl firmware image (musl + busybox + systemd)"
LICENSE = "MIT"

# sshd so we can bring the board up remotely. A getty on the mini-UART
# comes automatically: systemd's serial-getty generator reacts to the
# console= kernel cmdline arg (from SERIAL_CONSOLES + ENABLE_UART=1).
IMAGE_FEATURES += "ssh-server-dropbear"

# --- Bring-up access policy (DEV ONLY - rework before deployment) ----------
# UART:   empty root password + autologin root on the serial console, so a
#         UART cable alone gets you a root shell (nothing to type).
# SSH:    root-only, key-only - dropbear runs with -s (password logins
#         disabled; see dropbear.default) and only
#         /root/.ssh/authorized_keys exists (solar-rootkeys). Non-root
#         accounts have no key and no password.
# Before shipping: drop empty-root-password + serial-autologin-root, set a
# root password (or lock it), and keep key access only.
IMAGE_FEATURES += "allow-root-login empty-root-password serial-autologin-root"

# meta-raspberrypi adds 'kernel-modules' (= literally every kernel module,
# ~1800 packages) to MACHINE_EXTRA_RRECOMMENDS for every rpi machine, and
# BT firmware we don't use (no bluetooth distro feature). Drop both.
# Do NOT rely on MACHINE_EXTRA_RRECOMMENDS for WiFi firmware: the 2026-09-18
# image had linux-firmware-rpidistro-bcm43430 in that list and still omitted
# the package — install it explicitly. LICENSE_FLAGS_ACCEPTED is already set
# in kas-rpi0.yml.
MACHINE_EXTRA_RRECOMMENDS:remove = "kernel-modules bluez-firmware-rpidistro-bcm43430a1-hcd"

IMAGE_INSTALL:append = " \
    kernel-module-brcmfmac \
    linux-firmware-rpidistro-bcm43430 \
    wireless-regdb \
    wpa-supplicant \
    solar-wifi \
    solar-rootkeys \
"

# Bring-up/debug tooling for the RS485 Modbus bus (Deye inverter).
# USB serial driver packages cover the chips commonly used by USB-RS485
# adapters; drop whichever ones you don't need later. Remove mbpoll (GPL-3)
# from production images if its license doesn't suit distribution.
# rs485ctl toggles the kernel RS485 RTS direction control on ttyAMA0
# once the native transceiver is wired up (dtoverlay=solar-rs485).
IMAGE_INSTALL:append = " \
    mbpoll \
    rs485ctl \
    kernel-module-usbserial \
    kernel-module-ftdi-sio \
    kernel-module-cp210x \
    kernel-module-ch341 \
    kernel-module-pl2303 \
    kernel-module-cdc-acm \
"

# NV3007 2.79" TFT (dtoverlay=nv3007): generic panel-mipi-dbi driver +
# SPI host + GPIO backlight. Remaining deps (drm-mipi-dbi, kms helpers,
# fbdev emulation) come via the kernel module packages' own RDEPENDS.
# NOTE: the panel only comes up once the NV3007 init sequence is present
# as /lib/firmware/panel-mipi-dbi-spi.bin (vendor init code - see README).
IMAGE_INSTALL:append = " \
    kernel-module-panel-mipi-dbi \
    kernel-module-spi-bcm2835 \
    kernel-module-gpio-backlight \
"

# Placeholder for the inverter controller app once it lands:
# IMAGE_INSTALL:append = " inv-ctl"

COMPATIBLE_MACHINE = "^raspberrypi"
