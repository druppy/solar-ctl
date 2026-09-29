# solar-ctl U-Boot environment blob generator (no kconfig needed). libubootenv CANNOT create
# the env file: it must pre-exist at exactly CONFIG_ENV_SIZE (16 KiB in
# rpi_0_w_defconfig, CONFIG_ENV_FAT_FILE) with a valid header, or every
# fw_printenv/fw_setenv dies with "Cannot initialize environment"
# (bench 2026-09-25). Shipping it via IMAGE_BOOT_FILES makes p1 self-service.
#
# The blob MUST contain the FULL default environment plus slot=a - not just
# slot=a. U-Boot treats a valid-CRC env as the complete env: a slot-only blob
# silently deletes bootcmd/bootdelay (autoboot becomes a no-op, `boot` returns
# to the prompt - hit on the bench 2026-09-28 with the first factory blob;
# recovery was `env default -a` + `env save` from the U-Boot prompt). The
# default env text comes from u-boot's own deploy artifact
# (u-boot-initial-env-<machine>-<pv>, plain KEY=VALUE text, from u-boot.inc).
#
# MAINTENANCE: bitbake cannot hash another recipe's deployed file - after a
# u-boot version bump, touch this file so the blob is rebuilt.
#
# u-boot env blob format = 4-byte little-endian CRC32 over the remaining
# (size-4) bytes, then NUL-terminated name=value records, NUL-padded.
# zlib.crc32 matches u-boot's crc32 (same IEEE poly).
SUMMARY = "Seeded U-Boot environment (u-boot defaults + slot=a) for solar-ctl p1"
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
# Ordering only (reads DEPLOY_DIR_IMAGE, not the sysroot) - see MAINTENANCE
# note above for the hash limitation.
do_deploy[depends] += "virtual/bootloader:do_deploy"

python do_deploy() {
    import glob, os, struct, zlib
    size = int(d.getVar("UBOOT_ENV_SIZE"))
    pattern = os.path.join(d.getVar("DEPLOY_DIR_IMAGE"),
                           "u-boot-initial-env-%s-*" % d.getVar("MACHINE"))
    cands = sorted(set(os.path.realpath(p) for p in glob.glob(pattern)
                       if not p.endswith(".bin")))
    if len(cands) != 1:
        bb.fatal("expected exactly one u-boot initial-env at %s, found: %s"
                 % (pattern, cands))
    lines = []
    for line in open(cands[0]):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            bb.fatal("unexpected line in %s: %r" % (cands[0], line))
        if line.split("=", 1)[0] != "slot":  # ours goes last, always a
            lines.append(line)
    lines.append("slot=a")
    body = b"".join(l.encode() + b"\x00" for l in lines)
    if len(body) + 4 > size:
        bb.fatal("env body %d bytes larger than CONFIG_ENV_SIZE %d"
                 % (len(body) + 4, size))
    data = body.ljust(size - 4, b"\x00")
    blob = struct.pack("<I", zlib.crc32(data)) + data
    deploydir = d.getVar("DEPLOYDIR")
    os.makedirs(deploydir, exist_ok=True)
    bb.note("deploying %s/uboot.env (%d vars, %d bytes, from %s)"
            % (deploydir, len(lines), len(blob), os.path.basename(cands[0])))
    with open(os.path.join(deploydir, "uboot.env"), "wb") as f:
        f.write(blob)
}
do_deploy[umask] = "022"
addtask deploy before do_build after do_compile

COMPATIBLE_MACHINE = "^raspberrypi"
