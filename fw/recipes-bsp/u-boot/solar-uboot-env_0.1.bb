# solar-ctl U-Boot environment blob generator: the factory content of the
# RAW redundant env area (hidden SD region after p1; class solar-ablayout +
# doc ab-boot-uboot.md). Eaten by the wks via wic rawcopy, so a flashed card
# boots with a valid env immediately - libubootenv cannot create the env
# from zero (bench 2026-09-25), and on the raw area the same is true for
# U-Boot: without a valid CRC somewhere, boot.scr still works (defaults +
# its own slot fallback), but a card with garbage in the area would make
# fw_setenv fail its first read. Seeding costs nothing; skip it not.
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
# Redundant env blob format (byte layout VERIFIED against both sides of the
# wire, not guessed): u-boot include/env_internal.h env_t with
# CONFIG_ENV_REDUNDANT and libubootenv src/uboot_private.h uboot_env_redund
# are the same struct: 4-byte little-endian CRC32 over the data bytes only,
# then ONE flags byte, then NUL-terminated name=value records NUL-padded to
# (ENV_SIZE - 5). Two such copies concatenated: copy0 flags=0, copy1 flags=1;
# U-Boot's and libubootenv's env checks both pick the higher flags as current
# (copy1), so the very first saveenv/fw_setenv rewrites copy0 - the area is
# fully self-healing from boot one. zlib.crc32 matches u-boot's crc32 (same
# IEEE poly).
SUMMARY = "Seeded redundant U-Boot environment (u-boot defaults + slot=a)"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/COPYING.MIT;md5=3da9cfbcb788c80a0384361b4de20420"

INHIBIT_DEFAULT_DEPS = "1"
# provides DEPLOYDIR + sstate plumbing that lands the blob in DEPLOY_DIR_IMAGE
inherit deploy
# SOLAR_UBOOT_ENV_SIZE / SOLAR_UBOOT_ENV_BLOB (must track u-boot's
# CONFIG_ENV_SIZE for raspberrypi/rpi_0_w - 0x4000)
inherit solar-ablayout
do_fetch[noexec] = "1"
do_unpack[noexec] = "1"
do_configure[noexec] = "1"
do_compile[noexec] = "1"

# Ordering only (reads DEPLOY_DIR_IMAGE, not the sysroot) - see MAINTENANCE
# note above for the hash limitation.
do_deploy[depends] += "virtual/bootloader:do_deploy"

python do_deploy() {
    import glob, os, struct, zlib
    size = int(d.getVar("SOLAR_UBOOT_ENV_SIZE"))
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
    if len(body) + 5 > size:  # 5 = crc32(4) + flags(1) per copy
        bb.fatal("env body %d bytes larger than CONFIG_ENV_SIZE %d"
                 % (len(body) + 5, size))
    data = body.ljust(size - 5, b"\x00")
    crc = struct.pack("<I", zlib.crc32(data))
    blob = (crc + b"\x00" + data) + (crc + b"\x01" + data)
    if len(blob) != 2 * size:
        bb.fatal("internal: blob %d != 2 * %d" % (len(blob), size))
    deploydir = d.getVar("DEPLOYDIR")
    os.makedirs(deploydir, exist_ok=True)
    name = d.getVar("SOLAR_UBOOT_ENV_BLOB")
    bb.note("deploying %s/%s (%d vars, 2 x %d bytes, from %s)"
            % (deploydir, name, len(lines), size, os.path.basename(cands[0])))
    with open(os.path.join(deploydir, name), "wb") as f:
        f.write(blob)
}
do_deploy[umask] = "022"
# the python reads both SOLAR_* vars by NAME (d.getVar) = invisible to
# vartable hashing - pin them (the .rules do_swu_signing_keys trap):
do_deploy[vardeps] += "SOLAR_UBOOT_ENV_SIZE SOLAR_UBOOT_ENV_BLOB"
addtask deploy before do_build after do_compile

COMPATIBLE_MACHINE = "^raspberrypi"
