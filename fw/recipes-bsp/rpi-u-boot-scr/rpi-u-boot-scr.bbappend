# Swap the stock boot.cmd.in for the solar-ctl A/B script. The recipe's
# do_compile seds @@KERNEL_IMAGETYPE@@/@@KERNEL_BOOTCMD@@/@@BOOT_MEDIA@@
# out of ${UNPACKDIR}/boot.cmd.in, so the replacement file must keep that
# exact name; FILESEXTRAPATHS ordering decides which file://boot.cmd.in
# wins. See swupdate-ota.md §5 (per-slot kernel boot).
FILESEXTRAPATHS:prepend := "${THISDIR}/files/solar-ctl:"
