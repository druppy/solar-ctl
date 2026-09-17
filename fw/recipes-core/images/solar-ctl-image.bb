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
# SSH:    root-only, key-only - dropbear runs with -B (passwords refused;
#         see dropbear bbappend) and only /root/.ssh/authorized_keys exists
#         (solar-rootkeys). Non-root accounts have no key and no password.
# Before shipping: drop empty-root-password + serial-autologin-root, set a
# root password (or lock it), and keep key access only.
IMAGE_FEATURES += "allow-root-login empty-root-password serial-autologin-root"

# meta-raspberrypi adds 'kernel-modules' (= literally every kernel module,
# ~1800 packages) to MACHINE_EXTRA_RRECOMMENDS for every rpi machine, and
# BT firmware we don't use (no bluetooth distro feature). Drop both and
# install only what the Zero W actually needs; modules required to boot come
# from the kernel defconfig (built-in), and WiFi needs just brcmfmac +
# firmware (the bcm43430 firmware blobs come via the machine conf).
MACHINE_EXTRA_RRECOMMENDS:remove = "kernel-modules bluez-firmware-rpidistro-bcm43430a1-hcd"

IMAGE_INSTALL:append = " \
    kernel-module-brcmfmac \
    wireless-regdb \
    wpa-supplicant \
    solar-wifi \
    solar-rootkeys \
"

# Bring-up/debug tooling for the RS485 Modbus bus (Deye inverter).
# USB serial driver packages cover the chips commonly used by USB-RS485
# adapters; drop whichever ones you don't need later. Remove mbpoll (GPL-3)
# from production images if its license doesn't suit distribution.
IMAGE_INSTALL:append = " \
    mbpoll \
    kernel-module-usbserial \
    kernel-module-ftdi-sio \
    kernel-module-cp210x \
    kernel-module-ch341 \
    kernel-module-pl2303 \
    kernel-module-cdc-acm \
"

# Placeholder for the inverter controller app once it lands:
# IMAGE_INSTALL:append = " inv-ctl"

COMPATIBLE_MACHINE = "^raspberrypi"
