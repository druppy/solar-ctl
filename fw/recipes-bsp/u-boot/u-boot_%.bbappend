# solar-ctl U-Boot tweaks. u-boot comes from oe-core (always visible), so
# this bbappend is safe in the always-active fw/ layer (unlike the old
# swupdate one - see .rules dangling-bbappend gotcha).
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://solar-ctl.cfg"
