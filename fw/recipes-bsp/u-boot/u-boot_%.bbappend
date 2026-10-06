# solar-ctl U-Boot tweaks. u-boot comes from oe-core (always visible), so
# this bbappend is safe in the always-active fw/ layer (unlike the old
# swupdate one - see .rules dangling-bbappend gotcha).
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://solar-ctl.cfg"

inherit solar-ablayout

# Env config, post-merged (2026-10-05): the raw-MMC env switch AND the two
# offsets MUST come from solar-ablayout.bbclass (the wks and fw_env.config
# read the same constants), and SRC_URI *.cfg files are unpacked verbatim -
# bitbake never expands file contents - so the whole fragment rides in on a
# fragment generated at do_configure time. It cannot ride in via SRC_URI:
# unpacked files are verbatim (bitbake never expands file contents).
#
# TRAP (hit first run): the symbols must NOT ride in via SRC_URI *.cfg -
# u-boot-configure.inc merges those BEFORE its cml1 oldconfig
# (`yes '' | oe_runmake oldconfig`), and enabling ENV_IS_IN_MMC without
# values makes the defaultless hex ENV_OFFSET prompt forever (60 GB log,
# exit 137). So: prepend writes the fragment (fragment-only, no make);
# do_configure:append post-merges it over ${B}/.config and resolves with
# olddefconfig (NEVER oldconfig: promptless by design, cannot hang; our
# symbols carry explicit values, everything else keeps its default) - the
# last word on ${B}/.config (single-config rpi_0_w path; the multi-config
# UBOOT_CONFIG branch would need the ${B}/${builddir} form - unused on
# this machine). The greps then hard-fail rather than let a silently-dropped
# symbol ship an image whose env lives somewhere other than where the wks
# wrote it.
do_configure:prepend () {
    # ${B} exists here: do_configure[cleandirs] from u-boot-common.inc.
    printf '%s\n' \
        '# CONFIG_ENV_IS_IN_FAT is not set' \
        'CONFIG_ENV_IS_IN_MMC=y' \
        'CONFIG_ENV_REDUNDANT=y' \
        'CONFIG_ENV_OFFSET=${SOLAR_UBOOT_ENV_OFFSET_HEX}' \
        'CONFIG_ENV_OFFSET_REDUND=${SOLAR_UBOOT_ENV_OFFSET_REDUND_HEX}' \
        > ${B}/solar-env-redundant.cfg
}
# both SOLAR_UBOOT_ENV_*_HEX are expanded by bitbake at task-script
# emission (single quotes do NOT stop that - it is textual expansion, not
# shell), so the fragment is fully static by the time sh sees it.
do_configure:append () {
    if [ ! -e ${B}/.config ]; then
        bbfatal "solar-ctl: no ${B}/.config to post-merge env offsets into"
    fi
    # cwd of u-boot do_configure is ${B} (proven by uboot_configure()'s own
    # relative `merge_config.sh -m .config`); -O ${B} keeps that working.
    merge_config.sh -m -O ${B} ${B}/.config ${B}/solar-env-redundant.cfg
    oe_runmake olddefconfig
    grep -q '^CONFIG_ENV_IS_IN_MMC=y'            ${B}/.config || bbfatal "ENV_IS_IN_MMC dropped (unmet depends?)"
    grep -q '^CONFIG_ENV_REDUNDANT=y'            ${B}/.config || bbfatal "ENV_REDUNDANT dropped by olddefconfig"
    ! grep -q '^CONFIG_ENV_IS_IN_FAT=y'          ${B}/.config || bbfatal "ENV_IS_IN_FAT still on - FAT env would win"
    ! grep -q '^CONFIG_ENV_MMC_USE_DT=y'         ${B}/.config || bbfatal "ENV_MMC_USE_DT would override our raw offsets"
    grep -q '^CONFIG_ENV_OFFSET=${SOLAR_UBOOT_ENV_OFFSET_HEX}$'         ${B}/.config || bbfatal "ENV_OFFSET not ${SOLAR_UBOOT_ENV_OFFSET_HEX}"
    grep -q '^CONFIG_ENV_OFFSET_REDUND=${SOLAR_UBOOT_ENV_OFFSET_REDUND_HEX}$' ${B}/.config || bbfatal "ENV_OFFSET_REDUND not ${SOLAR_UBOOT_ENV_OFFSET_REDUND_HEX}"
}

# /etc/fw_env.config is owned by this recipe (${PN}-env package; oe-core
# u-boot.inc installs one if present, meta-raspberrypi's bbappend installs
# the /boot/uboot.env one on rpi). A postfunc runs after every do_install
# body INCLUDING both bbappends' :append fragments - unlike another
# do_install:append, it wins regardless of layer-priority bbappend ordering
# (our prio 6 < meta-raspberrypi's 9), and it keeps ownership in ${PN}-env
# so the image's existing u-boot-env install needs no change. Two lines =
# libubootenv's documented redundant pair (legacy format, "up to two
# entries are valid"); offsets are partition-absolute on the raw card, so
# fw_setenv/fw_printenv and SWUpdate's bootenv flush follow automatically.
do_install[postfuncs] += "solar_fw_env_config"
python solar_fw_env_config() {
    import os
    off = int(d.getVar("SOLAR_UBOOT_ENV_OFFSET_KB")) * 1024
    size = int(d.getVar("SOLAR_UBOOT_ENV_SIZE"))
    path = os.path.join(d.getVar("D") + d.getVar("sysconfdir"), "fw_env.config")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write("# solar-ctl: raw redundant U-Boot env area (hidden region after\n")
        f.write("# p1, written at flash time by wic; geometry from class\n")
        f.write("# solar-ablayout, doc docs/ab-boot-uboot.md). Two entries = the\n")
        f.write("# redundant pair. /boot is NOT involved - never mount p1 to switch.\n")
        f.write("/dev/mmcblk0 %#x %#x\n" % (off, size))
        f.write("/dev/mmcblk0 %#x %#x\n" % (off + size, size))
}
# both SOLAR_* vars are read by NAME inside python = invisible to vartable
# hashing (the do_swu_signing_keys stale-task trap; .rules) - pin them:
do_install[vardeps] += "SOLAR_UBOOT_ENV_OFFSET_KB SOLAR_UBOOT_ENV_SIZE"
