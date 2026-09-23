# solar-ctl SWUpdate build-time configuration.
#
# Scope: fw/kas-swupdate.yml builds this recipe to find out whether SWUpdate
# compiles on our musl/wrynose distro (fw/docs/swupdate-ota.md §10.1). It is
# deliberately NOT in any image yet - see the README's A/B updates section.
#
# swupdate.inc has no PACKAGECONFIG. Instead it inherits cml1, so any *.cfg
# file in SRC_URI is merged over the recipe's defconfig (merge_config.sh -m,
# last file wins) and then resolved by olddefconfig. That is what
# ${PN}/solar-ctl.cfg is for.
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI += "file://solar-ctl.cfg"

# kconfig silently drops a symbol whose "depends on HAVE_<lib>" probe failed
# (HAVE_* come from SWUpdate's own Makefile sysroot scan), so the MERGED
# ${B}/.config is the only evidence that a fragment line actually took effect.
# Snapshot it into ${T}: rm_work keeps `temp` by default
# (rm_work.bbclass: RM_WORK_EXCLUDE_ITEMS = "temp"), so this survives without
# excluding the whole recipe from removal.
# NB: do NOT go hunting for ${WORKDIR}/.config instead - that is the *input*
# aggregate (cflags + upstream defconfig), so it still contains the
# CONFIG_UBOOT=y/CONFIG_MTD=y lines our fragment removed and proves nothing.
do_configure:append() {
    if [ -f "${B}/.config" ]; then
        src="${B}/.config"
    elif [ -f "${S}/.config" ]; then
        src="${S}/.config"
    else
        src=""
        bbwarn "no merged kconfig found in ${B} or ${S}; CI verification will fail"
    fi
    if [ -n "$src" ]; then
        cp "$src" "${T}/swupdate-merged-dotconfig"
    fi
}
