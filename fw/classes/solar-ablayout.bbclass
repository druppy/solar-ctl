# solar-ctl A/B flash-layout constants - single source of truth (added
# 2026-10-05 when the U-Boot env moved off the p1 FAT partition).
#
# Inherited by exactly three recipes:
#   - solar-ctl-image.bb     (expands files/wic/solar-ctl-ab.wks.in here)
#   - u-boot_%.bbappend      (raw-env kconfig fragment + /etc/fw_env.config)
#   - solar-uboot-env.bb     (sizes the seeded env blob)
#
# The U-Boot env is a RAW redundant pair in a hidden (no partition-table
# entry) area of the SD card, flush with the end of p1 - NOT a FAT file.
# Why: every slot switch writes the env, and an ENV_IS_IN_FAT write mutates
# FAT metadata on p1 - the one region we treat as immutable (GPU firmware +
# u-boot.bin + boot.scr, no OTA path, corrupt = truck roll). The msdos
# table's four primary slots are already taken by p1..p4, hence --no-table
# (p2/p3/p4 keep their kernel-visible indexes). u-boot ENV_IS_IN_MMC +
# ENV_REDUNDANT and libubootenv both implement the standard redundant pair
# ([crc32 LE][flags u8][data], crc over the data only - u-boot
# env_internal.h ENV_HEADER_SIZE = 4+1, libubootenv uboot_env_redund), so a
# power cut mid-write always leaves one valid copy.
# Doc: docs/ab-boot-uboot.md (env area section).

# p1 geometry as written by the wks (wic --align start rounding):
# NB: the wks consumes SOLAR_BOOT_SIZE_MB as --fixed-size, NOT --size: wic
# multiplies plain --size parts by overhead_factor 1.3 while sizing the
# filesystem from content, which silently grew p1 100->130 MiB and collided
# with the hidden env area (do_image_wic "Could not place mmcblk02",
# 2026-10-05). --fixed-size bypasses that math, so the p1 end this class
# derives is what lands on the card.
SOLAR_BOOT_ALIGN_KB  = "4096"
SOLAR_BOOT_SIZE_MB   = "100"

# Per-copy env footprint == u-boot CONFIG_ENV_SIZE (rpi_0_w_defconfig:
# 0x4000). Keep in sync if the defconfig value ever moves.
SOLAR_UBOOT_ENV_SIZE = "16384"

# Hidden area sits flush with p1's end: align + boot size, in KiB (wic's
# --offset unit granularity). Growing the boot partition moves the area -
# and u-boot's ENV_OFFSETs + fw_env.config follow automatically, because
# everything derives from these two numbers.
SOLAR_UBOOT_ENV_OFFSET_KB = "${@int('${SOLAR_BOOT_ALIGN_KB}') + int('${SOLAR_BOOT_SIZE_MB}') * 1024}"
SOLAR_UBOOT_ENV_OFFSET_HEX = "${@'0x%x' % (int('${SOLAR_UBOOT_ENV_OFFSET_KB}') * 1024)}"
SOLAR_UBOOT_ENV_OFFSET_REDUND_HEX = "${@'0x%x' % (int('${SOLAR_UBOOT_ENV_OFFSET_KB}') * 1024 + int('${SOLAR_UBOOT_ENV_SIZE}'))}"

# Deploy artifact (solar-uboot-env) consumed by wic rawcopy
# (--sourceparams "file=...", resolved in DEPLOY_DIR_IMAGE):
SOLAR_UBOOT_ENV_BLOB = "uboot-env-redundant.bin"
