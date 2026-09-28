# Single signed .swu update artifact for the solar-ctl A/B layout
# (doc swupdate-ota.md §6). ONE file carries BOTH slots' payloads as
# libconfig "sets": `swupdate -i f.swu -e stable,main` writes rootfs->p2 +
# kernel->/data/cores/slot-a, `-e stable,alt` writes p3 + slot-b. The
# on-target wrapper /usr/bin/solar-update picks the set from /proc/cmdline.
#
# Not an image recipe - it only packages artifacts OTHER recipes deployed.
# Hence image-artifact-names (IMAGE_NAME/IMAGE_LINK_NAME, which the class
# uses for the .swu filename; without it you get a leading-dash name).
SUMMARY = "Signed single-file SWUpdate artifact for solar-ctl"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/COPYING.MIT;md5=3da9cfbcb788c80a0384361b4de20420"

inherit image-artifact-names
inherit swupdate-common
inherit solar-swu-signing

SRC_URI += "file://sw-description"

# Full DEPLOY_DIR_IMAGE entries, matched EXACTLY by swupdate-common
# (the stable symlink names, so no PK/PV/machine-suffix guessing). The
# squashfs rootfs comes from IMAGE_FSTYPES += "squashfs-xz" in
# solar-ctl-image.bb; uImage is the kernel U-Boot loads per-slot.
SWUPDATE_IMAGES = " \
    solar-ctl-image-raspberrypi0-wifi.rootfs.squashfs-xz \
    uImage \
"

# NO dtb in the update, deliberately: boot always uses the dtb + overlays
# the GPU firmware loads from p1, and p1 boot files are NEVER OTA'd (brick
# risk, .rules). Consequence: a kernel shipped in an update must stay
# compatible with the installed dtb.

# Task-order guarantee: everything SWUPDATE_IMAGES references must be
# deployed before do_swuimage runs (swupdate-common turns IMAGE_DEPENDS
# into do_swuimage[depends]).
IMAGE_DEPENDS = "solar-ctl-image"

INHIBIT_DEFAULT_DEPS = "1"
do_configure[noexec] = "1"
do_compile[noexec] = "1"

addtask swuimage before do_build

COMPATIBLE_MACHINE = "^raspberrypi"
