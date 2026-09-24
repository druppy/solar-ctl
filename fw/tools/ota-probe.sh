#!/bin/sh
# ota-probe.sh — read-only bench probe for the A/B OTA design (see
# fw/docs/swupdate-ota.md). Answers "what does this board actually tell us
# about its boot chain right now?" so the design does not have to guess.
#
# STRICTLY READ-ONLY: no mounts, no writes, no reboots, no module loading.
# Written for busybox ash (the target has no bash): no arrays, no [[, no
# process substitution, no $((...)) features beyond plain hex arithmetic.
#
# Usage:  scp fw/tools/ota-probe.sh root@<board>:/tmp/ && sh /tmp/ota-probe.sh
#
# Sections that exist specifically to feed the §8 bench protocol say so in a
# comment; everything else is test 0 (the baseline).

DT=/proc/device-tree

sec() {
	printf '\n===== %s =====\n' "$1"
}

show() {
	# show <file> [label] — dump a text sysfs/procfs file, or say absent
	if [ -f "$1" ]; then
		printf '  %-44s %s\n' "${2:-$1}" "$(cat "$1" 2>/dev/null | tr '\n' ' ')"
	else
		printf '  %-44s (absent)\n' "${2:-$1}"
	fi
}

kv() {
	# kv <file> <Key> — first "Key : value" line of a procfs-style file
	if [ -f "$1" ]; then
		v=$(grep -m1 "^$2" "$1" 2>/dev/null | tr -d '\r')
		printf '  %-44s %s\n' "$2" "${v:-$2: (not present)}"
	else
		printf '  %-44s %s (file absent)\n' "$2" "$1"
	fi
}

dt_u32() {
	# dt_u32 <devicetree-node> — decode a big-endian 32-bit cell.
	# /proc/device-tree leaves are binary; od -v stops od from collapsing
	# repeated bytes, tr -d ' \n' gives one bare hex string.
	if [ ! -f "$1" ]; then
		printf '  %-44s (absent)\n' "$1"
		return 0
	fi
	hex=$(od -An -tx1 -v "$1" 2>/dev/null | tr -d ' \n')
	if [ -z "$hex" ]; then
		printf '  %-44s (empty)\n' "$1"
		return 0
	fi
	printf '  %-44s 0x%s = %s\n' "$1" "$hex" "$((0x$hex))"
}

cfg() {
	# cfg <CONFIG_x> [...] — report each requested kernel option.
	if [ ! -f /proc/config.gz ]; then
		printf '  /proc/config.gz absent (enable CONFIG_IKCONFIG_PROC)\n'
		return 0
	fi
	for c in "$@"; do
		line=$(zcat /proc/config.gz 2>/dev/null | grep -E "^${c}[= ]")
		if [ -n "$line" ]; then
			printf '  %-34s %s\n' "$c" "$line"
		else
			printf '  %-34s NOT SET\n' "$c"
		fi
	done
}

sec "system"
kv /proc/cpuinfo Model
kv /proc/cpuinfo Revision
kv /proc/version Linux
cat /etc/version 2>/dev/null
printf '  uname: '; uname -a
if command -v systemctl >/dev/null 2>&1; then
	printf '  systemd: '; systemctl --version 2>/dev/null | head -n 1
	printf '  (need >= 254 for /run/systemd/reboot-param -> "0 tryboot")\n'
fi

sec "kernel command line"
cat /proc/cmdline 2>/dev/null

sec "Raspberry Pi firmware (device-tree /chosen)"
# The GPU firmware publishes /chosen/bootloader/{version,partnum}; version is
# the git commit of the boot firmware, partnum records the boot partition when
# autoboot.txt/boot_partition was used. Presence of the bootloader node at all
# is itself the answer to "is this firmware new enough to talk about slots".
if [ -d "$DT/chosen" ]; then
	ls -l "$DT/chosen" 2>/dev/null | sed 's/^/  /'
	find "$DT/chosen" -maxdepth 1 -type f 2>/dev/null | while read -r n; do
		dt_u32 "$n"
	done
fi
dt_u32 "$DT/chosen/bootloader/version"
dt_u32 "$DT/chosen/bootloader/partnum"
printf '  vcgencmd: '
if command -v vcgencmd >/dev/null 2>&1; then
	vcgencmd version 2>&1 | head -n 3
else
	printf '(not installed — libraspberrypi-bin is deliberately not in the image)\n'
fi

sec "mounts"
if command -v findmnt >/dev/null 2>&1; then
	findmnt 2>/dev/null | sed 's/^/  /'
else
	printf '  (findmnt absent — util-linux is not in the image; /proc/mounts follows)\n'
	sed 's/^/  /' /proc/mounts 2>/dev/null
fi
printf '  --- root/data/overlay lines ---\n'
grep -E ' /( |$)| /data| /etc| overlay| squashfs' /proc/mounts 2>/dev/null | sed 's/^/  /'

sec "filesystem support (/proc/filesystems)"
for fs in squashfs overlay ext4 vfat msdos; do
	if grep -qw "$fs" /proc/filesystems 2>/dev/null; then
		printf '  %-12s supported\n' "$fs"
	else
		printf '  %-12s MISSING\n' "$fs"
	fi
done

sec "kernel config"
cfg CONFIG_SQUASHFS CONFIG_SQUASHFS_XZ CONFIG_SQUASHFS_FILE_CACHE \
	CONFIG_OVERLAY_FS CONFIG_EXT4_FS CONFIG_MSDOS_PARTITION \
	CONFIG_RASPBERRYPI_FIRMWARE CONFIG_BCM2835_WDT CONFIG_MMC_BLOCK \
	CONFIG_BLK_DEV_INITRD

sec "block devices"
cat /proc/partitions 2>/dev/null | sed 's/^/  /'
printf '  --- /dev/mmcblk0p* ---\n'
ls -l /dev/mmcblk0p* 2>/dev/null | sed 's/^/  /'
for d in /dev/disk/by-label /dev/disk/by-partlabel /dev/disk/by-uuid /dev/disk/by-id; do
	printf '  --- %s ---\n' "$d"
	if [ -d "$d" ]; then
		ls -l "$d" 2>/dev/null | sed 's/^/    /'
	else
		printf '    (absent — by-partlabel needs a GPT)\n'
	fi
done
printf '  --- blkid ---\n'
if command -v blkid >/dev/null 2>&1; then
	blkid 2>/dev/null | sed 's/^/  /'
else
	printf '  (blkid absent)\n'
fi

sec "boot partition contents"
# wic's p1 is vfat; find where (if anywhere) it is mounted rather than
# assuming /boot, then look for the files the OTA design depends on.
# OTA_PROBE_BOOT overrides it, e.g. to inspect a card in a bench reader.
boot="$OTA_PROBE_BOOT"
[ -n "$boot" ] || boot=$(awk '$3=="vfat"{print $2; exit}' /proc/mounts 2>/dev/null)
[ -n "$boot" ] || boot=/boot
printf '  inspecting: %s\n' "$boot"
for f in config.txt tryboot.txt autoboot.txt cmdline.txt cmdline_a.txt \
	cmdline_b.txt kernel.img kernel8.img start.elf bootcode.bin; do
	if [ -e "$boot/$f" ]; then
		printf '  %-20s %s bytes\n' "$f" "$(wc -c < "$boot/$f" 2>/dev/null)"
	else
		printf '  %-20s (absent)\n' "$f"
	fi
done

sec "config.txt keys the OTA design leans on"
# Presence only: whether BCM2835 start.elf actually *honours* any of these is
# bench test 1/2 (fw/docs/swupdate-ota.md §8), which this script cannot decide.
# start.elf takes both `key=value` and `key value` (`include foo.txt`,
# `initramfs foo.cpio.gz`), so match the first token either way and print the
# original line. awk splits on leading whitespace for us (tabs collapsed first).
cfgkey() {
	tr '\t' ' ' < "$boot/config.txt" 2>/dev/null | awk -v k="$1" '
		{ key = $1; sub(/[ =].*/, "", key)
		  if (tolower(key) == tolower(k)) { sub(/^ +/, "", $0); printf "%s; ", $0 } }' \
		| sed 's/; $//'
}
if [ ! -f "$boot/config.txt" ]; then
	printf '  (no config.txt under %s — cannot answer tests 1/2)\n' "$boot"
else
	for k in kernel kernel8 initramfs os_prefix cmdline cmdline_file dtoverlay \
		include boot_partition tryboot_a_b autoboot enable_uart; do
		found=$(cfgkey "$k")
		if [ -n "$found" ]; then
			printf '  %-15s %s\n' "$k" "$found"
		else
			printf '  %-15s (not set)\n' "$k"
		fi
	done
	secs=$(tr '\t' ' ' < "$boot/config.txt" | awk '{ if ($1 ~ /^\[/) printf "%s ", $1 }')
	printf '  sections:   %s\n' "${secs:-(none — no [all]/[tryboot] stanzas)}"
fi

sec "p1 write path (the slot-switch commit mechanism)"
# Every tier switches slots by rewriting one text file on the FAT partition,
# so what matters is how p1 is mounted right now and how much room it has.
conf_mount=$(awk -v m="$boot" '$2==m {print; exit}' /proc/mounts 2>/dev/null)
printf '  p1 mount: %s\n' "${conf_mount:-(not mounted; free space unknown — this script will not mount it)}"
opt=""
[ -n "$conf_mount" ] && opt=$(printf '%s\n' "$conf_mount" | awk '{print $4}')
if printf '%s' ",$opt," | grep -q ',ro,'; then
	printf '  p1 is READ-ONLY: a switch needs mount -o remount,rw (bench test 3)\n'
elif [ -n "$opt" ]; then
	printf '  p1 is WRITABLE (%s): switch is a plain write — but that is also a risk\n' "$opt"
fi
printf '  root mount options: %s\n' "$(awk '$2=="/"{print $4; exit}' /proc/mounts 2>/dev/null)"
# Only df the real mount: df on an unmounted $boot would report the rootfs.
if [ -n "$conf_mount" ] && command -v df >/dev/null 2>&1; then
	df -k "$boot" 2>/dev/null | sed 's/^/  /'
fi

sec "SD card size (layout sizing)"
sd=""
rootdev=$(tr ' ' '\n' < /proc/cmdline 2>/dev/null | grep -m1 '^root=')
printf '  kernel says:  %s\n' "${rootdev:-(no root= — an initramfs decides)}"
for b in /sys/block/mmcblk0 /sys/block/mmcblk1 /sys/block/sda; do
	[ -d "$b" ] || continue
	sd=yes
	sz=$(cat "$b/size" 2>/dev/null)
	if [ -n "$sz" ]; then
		printf '  %-20s %s 512B blocks = %s MiB\n' "$b" "$sz" "$((sz / 2048))"
	else
		printf '  %-20s (size unreadable)\n' "$b"
	fi
	show "$b/removable" "$b removable"
done
[ -n "$sd" ] || printf '  (no mmcblk0/mmcblk1/sda — no SD-card device to size)\n'

sec "arming tryboot from userspace"
# systemd >= 254 arms tryboot via /run/systemd/reboot-param. If it is too old,
# the fallback is the same mailbox property the kernel driver issues
# (RPI_FIRMWARE_SET_REBOOT_FLAGS) over /dev/vcio — same shape as rs485ctl.
if [ -c /dev/vcio ]; then
	printf '  /dev/vcio: present (mailbox reachable from userspace)\n'
	printf '           -> SET_REBOOT_FLAGS wrapper is the systemd-free route\n'
else
	printf '  /dev/vcio: absent (no userspace mailbox route; systemd route only)\n'
fi
if [ -d /run/systemd ]; then
	show /run/systemd/reboot-param "/run/systemd/reboot-param (armed now?)"
else
	printf '  /run/systemd absent (not a systemd system)\n'
fi

sec "SWUpdate / OTA status"
for c in swupdate swupdate-client; do
	printf '  %-18s ' "$c"
	if command -v "$c" >/dev/null 2>&1; then
		command -v "$c"
	else
		printf '(not installed)\n'
	fi
done
show /etc/hwrevision "hwrevision (SWUpdate HW_COMPATIBILITY)"
for s in /tmp/sockinstctrl /tmp/swupdateprog; do
	if [ -e "$s" ]; then printf '  %-18s present\n' "$s"; else printf '  %-18s absent\n' "$s"; fi
done

sec "memory (why installs must stream, not extract)"
if command -v free >/dev/null 2>&1; then
	free 2>/dev/null | sed 's/^/  /'
fi
grep -E '^(MemTotal|SwapTotal)' /proc/meminfo 2>/dev/null | sed 's/^/  /'
grep -E ' /(tmp|run) ' /proc/mounts 2>/dev/null | sed 's/^/  /'

printf '\n===== done (nothing was written) =====\n'
