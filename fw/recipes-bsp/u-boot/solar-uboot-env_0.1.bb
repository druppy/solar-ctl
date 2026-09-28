# Pre-seeded U-Boot environment blob for /boot/uboot.env.
#
# libubootenv CANNOT create the env file: it must pre-exist at exactly
# CONFIG_ENV_SIZE (16 KiB in rpi_0_w_defconfig, CONFIG_ENV_FAT_FILE) with a
# valid header, or every fw_printenv/fw_setenv dies with "Cannot initialize
# environment" (bench 2026-09-25, where we seeded it by hand with dd).
# Shipping it via IMAGE_BOOT_FILES makes first boot self-service.
#
# u-boot env blob format = 4-byte little-endian CRC32 over the remaining
# (size-4) bytes, then NUL-terminated name=value records, NUL-padded.
# zlib.crc32 matches u-boot's crc32 (same IEEE poly).
SUMMARY = "Seeded U-Boot environment (slot=a) for solar-ctl p1"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/COPYING.MIT;md5=3da9cfbcb788c80a0384361b4de20420"

INHIBIT_DEFAULT_DEPS = "1"
# provides DEPLOYDIR + sstate plumbing that lands the blob in DEPLOY_DIR_IMAGE
inherit deploy
do_fetch[noexec] = "1"
do_unpack[noexec] = "1"
do_configure[noexec] = "1"
do_compile[noexec] = "1"

# Must track u-boot's CONFIG_ENV_SIZE for raspberrypi/rpi_0_w.
UBOOT_ENV_SIZE = "16384"

python do_deploy() {
    import os, struct, zlib
    size = int(d.getVar("UBOOT_ENV_SIZE"))
    body = b"slot=a\x00"
    if len(body) + 4 > size:
        bb.fatal("env body larger than CONFIG_ENV_SIZE")
    data = body.ljust(size - 4, b"\x00")
    blob = struct.pack("<I", zlib.crc32(data)) + data
    deploydir = d.getVar("DEPLOYDIR")
    os.makedirs(deploydir, exist_ok=True)
    bb.note("deploying %s/uboot.env (%d bytes)" % (deploydir, len(blob)))
    with open(os.path.join(deploydir, "uboot.env"), "wb") as f:
        f.write(blob)
}
do_deploy[umask] = "022"
addtask deploy before do_build after do_compile

COMPATIBLE_MACHINE = "^raspberrypi"
