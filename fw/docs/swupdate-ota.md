# A/B OTA updates for solar-ctl (SWUpdate)

**Status: design record + musl compile gate PASSED + bench tests 0–4 DONE.**
Nothing A/B in an image yet. Plan of record: **per-slot kernel via U-Boot
(Tier 3)** — one slot = one kernel + modules + root (§5). Branch
`swupdate_setup`. SWUpdate builds on musl (`[ci]` run 35926622985).
This file is the research + design record so the next session does not have
to re-derive any of it. The bench protocol that settles the remaining unknowns
is §8, and the tool that starts it is `fw/tools/ota-probe.sh`. Facts carry a provenance label:

| Label        | Meaning                                                            |
| ------------ | ------------------------------------------------------------------ |
| `[wrynose]`  | Verified against the oe-core/meta-raspberrypi `wrynose` branch     |
| `[master]`   | Verified against upstream `master` (wrynose may lag — re-check)    |
| `[kas]`      | Read from kas' own sources (build-tool behaviour, not a layer)     |
| `[ci]`       | Observed in our own Actions run (job log / uploaded artifact)      |
| `[reported]` | From upstream docs/a source I read, but not re-read first-hand     |
| `[inferred]` | My reasoning from the above — treat as a hypothesis to test        |
| `[bench]`    | Unknown today; settles only by running the probe/tests on hardware |

---

## 1. Goal

Replace the current "one ext4 rootfs, reflash to update" model with a
fail-safe OTA scheme:

- two **read-only** root slots (A/B), contents shipped as **squashfs (xz)**
- **`/etc` writable** as an overlay (factory-reset-able, survives slot switch)
- a **`data`** partition (ext4, journalled) for the app's persistent state
- **SWUpdate** as the update agent; slot selection done by the **Raspberry Pi
  firmware** (there is no U-Boot on this machine and we are not adding one)
- delivery via the **SWUpdate web UI** first, **Eclipse Hawkbit** later

Hard constraints from the hardware: **BCM2835, 512 MB RAM, no RTC, SD-card
mass storage, one FAT boot partition that the GPU firmware must read.**

---

## 2. Boot chain as it exists today

```
SoC bootcode.bin → start.elf (GPU firmware, reads config.txt)
                 → kernel8.img / kernel.img + .dtb  (start.elf loads these)
                 → kernel with cmdline.txt          (root=…, console=…)
                 → /init → systemd
```

- `WKS_FILE ?= "sdimage-raspberrypi.wks"` — p1 vfat (boot, ~100 MB) +
  p2 ext4 (rootfs). That's the whole "partition setup" today. `[wrynose]`
- `cmdline.txt` is *generated*, not a static file:
  `recipes-bsp/bootfiles/rpi-cmdline.bb`, with
  `CMDLINE_ROOT_PARTITION ?= "/dev/mmcblk0p2"`, `CMDLINE_ROOT_FSTYPE`,
  `CMDLINE_SERIAL`, deployed into `${BOOTFILES_DIR_NAME}`. `[wrynose]`
- Boot files themselves come from `rpi-bootfiles.bb`, `PV = ${RPIFW_DATE}`.
- Canned kickstart files now live in **`<layer>/files/wic/`**:
  `layers/meta-raspberrypi/files/wic/sdimage-raspberrypi.wks`. `[wrynose]`

### 2.1 `wic` moved out of oe-core (found 2026-09-23)

`scripts/lib/wic/` **no longer exists** in oe-core — neither `wrynose` nor
`master` (every `rawcopy.py` fetch 404s; `scripts/lib/` listings confirm).
It is now a **standalone, separately versioned upstream project** consumed by
a normal recipe:

- `meta/recipes-support/wic/wic_0.3.1.bb` `[wrynose]`
  - `HOMEPAGE = "https://git.yoctoproject.org/wic"`
  - `SRC_URI = "git://git.yoctoproject.org/wic.git;branch=master;protocol=https;tag=v${PV}"`
  - `SRCREV = "6c66da65bf2aac618092811c13792dcbd453d140"`, `BBCLASSEXTEND = "native nativesdk"`
- `image_types_wic.bbclass` drives it exactly as before (`wic create …`), and
  `do_image_wic[depends]` now includes **`wic-native`** (and `wic`),
  plus `parted gptfdisk dosfstools mtools`. `[wrynose]`
- New in the class: an **imager** abstraction — `IMAGER=direct` default,
  `--imager` parsed out of `WIC_CREATE_EXTRA_ARGS`, and the result is renamed
  `*.${IMAGER}` → `.wic`. `[wrynose]`
- `WKS_SEARCH_PATH ?= "${THISDIR}:${@':'.join('%s/files/wic' % p for p in '${BBPATH}'.split(':'))}"`
  → **our new `.wks` must live in `fw/files/wic/`**, not `fw/wic/`. `[wrynose]`
- `.wks.in` templating still exists (`do_write_wks_template`, and the
  `bb.parse.mark_dependency` + `do_write_wks_template` task insertion in the
  `python ()` anonymous block). `[wrynose]`

**Consequences for us:**

1. Our workflow is unchanged (`wic.bz2` still appears) — this is a *relocated*
   tool, not a replacement. Neither "switched to another tool" nor "integrated
   into the image build".
2. **Fetch risk:** `wic-native` is fetched from `git.yoctoproject.org`, which
   our `.rules` record as Cloudflare-403. **`[bench]` 2026-09-24 — PASSED:**
   `bitbake wic-native -c fetch` succeeded on the build host (clone now at
   `build/downloads/git2/git.yoctoproject.org.wic.git`, tags `v0.3.0` and
   `v0.3.1` present), so smart-HTTPS git works even where the cgit web UI
   returns a Cloudflare challenge. The risk stands — one Cloudflare policy
   change from breaking every build — and the escape hatch remains unwritten:
   a `wic_%.bbappend` pointing `SRC_URI` at a GitHub fork we control (same
   trick as the layer mirrors).
3. To read plugin behaviour (`rawcopy`, `bootimg-pcbios`, …) we can no longer
   grep `layers/openembedded-core/scripts/lib/wic`. It is in the **wic repo**,
   and after a build the only local copy is the download mirror, which
   `rm_work` leaves alone:

   ```sh
   git --no-pager -C build/downloads/git2/git.yoctoproject.org.wic.git log --oneline -n 3
   git --no-pager -C build/downloads/git2/git.yoctoproject.org.wic.git show v0.3.1:src/wic/plugins/source/rawcopy.py
   ```

   `[bench]` 2026-09-24: path confirmed — it is **`src/wic/plugins/source/rawcopy.py`**;
   the earlier `lib/wic/…` guess (`[inferred]` from the old oe-core layout) was
   wrong. `git ls-tree -r --name-only v0.3.1 | grep -i rawcopy` returns exactly
   that one plugin (plus `pluginbase.py` / `plugins/imager/direct.py`).

---

## 3. Slot switching without a bootloader

No U-Boot ⇒ the **RPi firmware owns which root we boot**. Two mechanisms:

### 3.1 `tryboot` (one-shot A/B fallback) — the workhorse

- `[wrynose]` verified in the kernel: `drivers/firmware/raspberrypi.c` parses
  `reboot="0 tryboot"` and issues `RPI_FIRMWARE_SET_REBOOT_FLAGS`; the driver
  binds on `brcm,bcm2835-firmware` — i.e. **available on the Zero W**.
- `[wrynose]` upstream docs state **"All Raspberry Pi models support
  `tryboot`"** (`config_txt/bootflow-eeprom.adoc`); the only caveat recorded
  there is EEPROM write-protect on Pi 4B rev 1.0/1.1 — not our board.
- Semantics: firmware sets a **one-shot** flag, clears it during boot, and on
  the *next* boot loads **`tryboot.txt` instead of `config.txt`**. So
  `tryboot.txt` is a complete alternative `config.txt`.
- Arm it from Linux (systemd ≥ 254, which HAOS uses exactly this way):

  ```sh
  echo '0 tryboot' > /run/systemd/reboot-param && systemctl reboot
  ```

### 3.2 `autoboot.txt` (partition-select style) — attractive, now PROVEN on BCM2835

`[wrynose]` `config_txt/autoboot.adoc`: `boot_partition=<n>` plus
`tryboot_a_b=1` and a `[tryboot]` section, with a six-partition reference
layout and a "commit by rewriting the file" flow. The `[tryboot]` section,
`cmdline=` and `os_prefix` carry **no model restriction** in the docs.
**`[bench]` 2026-09-24 test 2: the legacy BCM2835 `start.elf` (20250801) DOES
parse `autoboot.txt` and honour `[all]`/`[tryboot] boot_partition=`** — the
`partition` u32 in `/chosen/bootloader` tracks the choice (`0` with no file,
`1` with `[all]=1`, `9` with armed `[tryboot]=9`); an impossible target did not
hang the boot, the firmware fell back to tryboot.txt→p1 in the same boot.
`partnum`/`version` never appear on this firmware — the DT channel is
`partition`, not `partnum`. Still unproven: that boot *lands* on a second FAT
partition chosen by `boot_partition` (bench SD has only one FAT partition).
Do **not** extrapolate from `boot_count`/`boot_arg1` (Pi 5 only) or
`bootvar0` (Pi 4+).

### 3.3 Bootloader version — yes, worth checking

- The firmware we ship is pinned by the layer branch: `rpi-bootfiles.bb`
  `PV = ${RPIFW_DATE}` (master: `20260521`, `SRCREV 09267f5354…`; the
  `wrynose` branch will be **older**). `[master]` for the master numbers.
- Cheapest check on the build host, no target needed:

  ```sh
  bitbake -e rpi-bootfiles | grep -E '^(PV|SRCREV|RPIFW_DATE)='
  ```

  or read the `rpi-bootfiles-<PV>.stamp` left in
  `build/tmp/deploy/images/raspberrypi0-wifi/` (survives `rm_work`).
- On target, `vcgencmd version` would print it, but that needs
  `libraspberrypi-bin`, which we deliberately do not install.
- If a bench test needs newer firmware: a small `rpi-bootfiles_%.bbappend`
  overriding `SRCREV` + `RPIFW_DATE` is the legitimate lever.

---

## 4. Filesystem decisions

| Area     | Decision                                      | Why                                                             |
| -------- | --------------------------------------------- | --------------------------------------------------------------- |
| Root A/B | **squashfs-xz**                               | RO by construction, no fsck, ~50 % smaller. Kernel is *not* loaded from here (U-Boot xz) |
| `root=`  | **`/dev/mmcblk0pN`** device nodes             | matches meta-raspberrypi's own `CMDLINE_ROOT_PARTITION` default |
| `/etc`   | OE-core **`overlayfs-etc`** image feature     | upstream, systemd-aware, factory-reset = wipe upper dir         |
| `/var`   | explicit overlay via **`overlayfs.bbclass`**  | systemd never runs OE's initscripts volatile hook               |
| `/data`  | **ext4 with journal**                         | power-loss safety beats write-count worries for app state       |
| p1 FAT   | boot files only, writes must stay **rare**    | FAT has no journal; never keep a status DB there                |

Verified upstream plumbing:

- `image_types.bbclass` provides `squashfs`, `squashfs-xz`, `squashfs-lzo`,
  `squashfs-lz4`, `squashfs-zst` through `oe_mksquashfs` (+ `erofs*`).
  Plain `squashfs` = mksquashfs default compression; use **`squashfs-xz`** to
  be explicit. `[master]` — re-confirm the list on wrynose locally.
- **`overlayfs-etc.bbclass`** (`meta/classes-recipe/`, image feature
  `overlayfs-etc`). Vars: `OVERLAYFS_ETC_DEVICE`, `_MOUNT_POINT`, `_FSTYPE`,
  `_MOUNT_OPTIONS`, `_EXPOSE_LOWER`, `_CREATE_MOUNT_DIRS`,
  `_USE_ORIG_INIT_NAME`. **Conflicts with the `package-management` image
  feature.** Preinit script `meta/files/overlayfs-etc-preinit.sh.in` creates
  `<mnt>/overlay-etc/{upper,work}`, mounts the overlay over `/etc`, then
  execs `/sbin/init.orig`. `[wrynose]`
  - ⚠ **`OVERLAYFS_ETC_CREATE_MOUNT_DIRS` default `1` does
    `mount -o remount,rw /`** — on a squashfs root that can *never* succeed.
    Set `OVERLAYFS_ETC_CREATE_MOUNT_DIRS = "0"` and pre-create `/data` in the
    rootfs. `[wrynose]`
- **`overlayfs.bbclass`** (for `/var` and friends):
  `REQUIRED_DISTRO_FEATURES += "systemd overlayfs"` → **`overlayfs` is not in
  our `DISTRO_FEATURES_DEFAULTS`** (`fw/conf/distro/solar-ctl.conf` line 23)
  and must be added; the kernel also needs `CONFIG_OVERLAY_FS`. Refuses to
  manage `/etc` by design (that's `overlayfs-etc`'s job). QA escape hatch:
  `OVERLAYFS_QA_SKIP[data] = "mount-configured"`. `[wrynose]`
- `/var/volatile` handling for RO rootfs lives in **initscripts**, which
  systemd never runs → we must provide the overlay/tmpfiles ourselves.
- **Why xz is not a RAM problem (checked 2026-09-25)**: squashfs
  decompression is per-block and single-buffered — ~150 KiB transient peak
  for a 128 KiB block, freed after each read; page-cache cost is identical
  for all algorithms. xz's real cost is one-time CPU per file (slowest on
  ARMv6); its benefit is ~2× smaller slot/`.swu` writes. All three
  decompressors are already in the defconfig (`SQUASHFS_XZ/LZO/ZSTD=y`)
  `[bench]` kernel 6.18.33 — if boot CPU ever shows up, `zstd` (near-xz
  size, ~2–3× faster) is a one-word wks change.

---

## 5. Target disk layout

Partition table stays **MSDOS** (GPT has been reported to upset legacy Pi
boot). Use ext4 **fs labels** (`/dev/disk/by-label/…`); `by-partlabel` needs
GPT. Squashfs slots are **fixed size ≥ payload** (1 KB short = failed install).
Never OTA `bootcode.bin`/`start.elf`. Never keep a status DB on FAT.

Today's card (2026-09-18 image): p1 vfat ~130 MB + p2 ext4 **135 MB** on a
122 GB disk — resize never ran. Any A/B layout must grow slots at flash time
(wic sizes), not first-boot `resize2fs` of a squashfs.

```
             what start.elf can do (firmware 20250801)
  tryboot.txt     YES  (1a: boot_delay Δ 5 s)
  cmdline=        NO   (1b: file loaded, /proc/cmdline unchanged)
  kernel=         NO   (1c: missing name still booted)
  autoboot.txt    YES  parse + [tryboot] section (partition 1→9)
                  landing on a *second FAT* still unproven
```

### Comparison

| | Classic Tier 1 | Tier 1-alt | Tier 2 | **Tier 3 U-Boot (plan of record)** |
| -- | -- | -- | -- | -- |
| Slot pick | `cmdline=` / `kernel=` in tryboot.txt | initramfs marker on `/data` | `autoboot.txt` `boot_partition=` | U-Boot `bootcmd` / `altbootcmd` |
| Kernel vs modules | one kernel on p1 | same (§9 hazard) | per-FAT *if* landing works | **one kernel+modules per slot** |
| Firmware coop | required — **dead on BCM2835** | none | parse proven; land unproven | `start.elf` only loads `u-boot.bin` |
| FAT copies | 1 | 1 | 2 | 1 (GPU firmware + U-Boot only) |
| Commit | rewrite tryboot.txt | write `/data/slot` | rewrite `autoboot.txt` | `fw_setenv` on `/data` |

`start.elf` cannot load a kernel out of a rootfs. The only way a slot is a
single understandable unit (kernel + modules + root together) is a **second
stage that *can* read the slot filesystem**. That is U-Boot.

### Recommended: per-slot kernel (U-Boot)

One FAT that never changes after factory flash; two slot partitions that each
hold a complete bootable Linux; `/data` for app state, overlay upper, and the
U-Boot env (so `bootcount` is **not** a FAT write every boot).

```
 p1  ~64 MB    vfat        boot      bootcode.bin, start.elf, config.txt
                                     kernel.img = u-boot.bin, boot.scr
                                     slot-a/{zImage,dtb}  slot-b/{zImage,dtb}
                                     NEVER OTA bootcode/start.elf/u-boot.bin
 p2  ~350 MB   squashfs-xz rootfs-a  root + /lib/modules for kernel-a
 p3  ~350 MB   squashfs-xz rootfs-b  same for B
 p4  rest      ext4        data      /data — app, overlay upper, uboot.env
```

Boot:

```
start.elf → u-boot.bin (as kernel.img)
  → boot.scr reads env on /data
  → load zImage+dtb from p1 slot-${slot}/  (fatload, not squashfs)
  → bootz root=/dev/mmcblk0p{2|3} rootfstype=squashfs
  → overlayfs-etc (CREATE_MOUNT_DIRS=0); solar-rs485 takes GPIO14/15
```

**p2/p3 are squashfs-xz.** That is the right root (RO, no journal, small).
U-Boot on this board is not assumed to read xz squashfs, so it does **not**
`load` the kernel out of p2/p3. The matching `zImage`+dtb for each slot live
as files on p1 (`slot-a/`, `slot-b/`). Modules stay *inside* the squashfs, so
kernel and modules still ship together in one `.swu`:

1. write inactive squashfs (`installed-directly=true`)
2. write that slot’s `zImage`+dtb on p1 (only those files, never `u-boot.bin`)
3. `fw_setenv` to the new slot

Interrupt before step 3 → still booting the old slot. Size squashfs slots **at
wic time** (≥ payload + headroom); you cannot `resize2fs` them.

SWUpdate writes the **inactive** slot (`installed-directly=true`), then
`fw_setenv` to point at it and set `upgrade_available`. `bootcount` +
`altbootcmd` is mainline (not a 2016 demo). Rollback = boot the other label.

UART / MAX485 (hardware, not a U-Boot patch):

- `ENABLE_UART=1` stays (meta-rpi `bbfatal` if U-Boot + UART=0 on this machine).
- Split DE vs /RE: GPIO17/RTS → **DE** with pulldown; **/RE** pulled high at
  reset. Boot: chip deaf+mute, GPIO14/15 is a clean console for U-Boot.
- Production `boot.scr`: `bootdelay=-2` so bus noise never aborts autoboot.
- After `bootz`, `solar-rs485` + `rs485ctl` enable the transceiver; getty off.

Do **not** OTA `u-boot.bin` / `start.elf` / `bootcode.bin`. A U-Boot bump is a
bench job, same class as GPU firmware.

### Fallback: Tier 1-alt (initramfs, shared kernel on p1)

Proven on the bench if we later refuse a second stage. Firmware always reads
the same p1; userspace picks the squashfs. Kernel/modules coherence is a live
§9 hazard — pin `SRCREV`. Not the plan of record.

### Fallback: Tier 2 (two FAT)

Only if we first prove *landing*: a card with two FAT partitions, distinct
`cmdline.txt` (e.g. `slot=A` vs `slot=B`), `boot_partition=` flip, and
`/proc/cmdline` actually changes. Until that test, do not build this wks.

```
 p1  ~100 MB  vfat      boot-a    GPU firmware + kernel_A + cmdline_A
 p2  ~100 MB  vfat      boot-b    kernel_B + cmdline_B  (± copy of start.elf?)
 p3  ~350 MB  squashfs  rootfs-a
 p4  ~350 MB  squashfs  rootfs-b
 p5  rest     ext4      data      /data as above
```

`autoboot.txt` on whichever FAT the firmware reads first:

```
[all]
boot_partition=1

[tryboot]
boot_partition=2
```

Commit = rewrite `boot_partition` + fsync. Open question the landing test
must answer: does p2 need its own `start.elf`, or is p1's GPU firmware
enough? **Do not copy `bootcode.bin` onto p2 as an OTA path.**

### U-Boot facts on wrynose (still true)

| Fact                                                                                                              | Source                            |
|-------------------------------------------------------------------------------------------------------------------|-----------------------------------|
| `raspberrypi0-wifi.conf` sets `UBOOT_MACHINE ?= "rpi_0_w_defconfig"`                                              | machine conf                      |
| u-boot is gated behind `RPI_USE_U_BOOT = "1"` (unset ⇒ firmware boots)                                            | `rpi-base.inc`                    |
| enabled ⇒ `KERNEL_IMAGETYPE`→`uImage`, `u-boot.bin;${SDIMG_KERNELIMAGE}` and `boot.scr` join `IMAGE_BOOT_FILES`   | `rpi-base.inc`                    |
| `RPI_USE_U_BOOT=1` + `ENABLE_UART=0` is a **hard `bbfatal`**; otherwise it force-appends `enable_uart=1`          | `rpi-config_git.bb:191-200`       |
| stock `rpi-u-boot-scr` `boot.cmd.in` has no A/B / bootcount; bootargs from DTB `/chosen`                          | meta-rpi                          |
| smoke image: `U-Boot 2026.01`, `bootcmd=bootflow scan`, `bootdelay=2`, prompt `U-Boot>`; banner → kernel in ~6 s, shell in ~40 s; any input byte inside the 2 s window interrupts autoboot (board waits at the prompt — not bricked) | `[bench]` 2026-09-25 |

Keep `ENABLE_UART=1` (already on this image). Console on 14/15 during U-Boot is
**wanted** for the bench; MAX485 isolation (DE pulldown, /RE pull-up) plus
`bootdelay=-2` is what keeps the inverter bus from aborting autoboot. The
per-slot `boot.cmd` is ours. Never OTA `u-boot.bin`. §6.7 has the 2016-demo
comparison (mainline already has `bootcount`/`altbootcmd`).

---

## 6. SWUpdate plan

### 6.1 Layer & recipe

Implemented on the `swupdate_setup` branch as a **compile gate only**: added in
`fw/kas-swupdate.yml` (which `includes: fw/kas-rpi0.yml` and replaces `target:`
with `swupdate`), *not* in `kas-rpi0.yml` — the image build stays untouched
until the musl question is answered. Built by the `swupdate` CI job.

> **Dangling-bbappend trap (learned the hard way, PR #1 CI).** Our recipe glue
> first lived in `fw/recipes-support/swupdate/` and the image build died:
> `ERROR: No recipes in default available for: .../swupdate_%.bbappend`.
> Every layer is parsed by every build, so a `*.bbappend` in an active layer
> whose target recipe is absent is a **hard error**, not a warning — even
> though nothing in the image references swupdate. Fix: a separate thin layer
> `fw-swupdate/` (`conf/layer.conf`, collection `solar-ctl-swupdate`,
> `LAYERREQUIRES += "swupdate"`) that *only* `fw/kas-swupdate.yml` adds via
> `repos.solar-ctl.layers`, which is a mapping so it merges with the included
> config's `fw`. When SWUpdate enters the image, move the recipe back into
> `fw/` and delete the layer. `[wrynose]`

- `meta-swupdate` has a **`wrynose`** branch; `BBFILE_COLLECTIONS += "swupdate"`,
  priority 6, `LAYERSERIES_COMPAT_swupdate = "whinlatter wrynose"`,
  `LAYERDEPENDS_swupdate = "openembedded-layer"` — already satisfied, we carry
  `meta-oe`. The layer lives at the **repo root** (`conf/layer.conf` at top
  level), so the kas entry needs no `layers:` key — same shape as our
  `meta-raspberrypi` entry. GitHub mirror `sbabic/meta-swupdate`. `[wrynose]`
- Recipes on the branch: `swupdate.inc`, one shared `swupdate/` file dir (one
  `defconfig` for all versions), `swupdate_2025.12.bb`, `swupdate_2026.05.1.bb`,
  `swupdate_git.bb`. Default provider resolves to the highest PV,
  **2026.05.1**; we do not pin. `[wrynose]`
- **No `PACKAGECONFIG`.** `swupdate.inc` inherits `cml1`: `do_configure` writes
  `CONFIG_EXTRA_CFLAGS/LDFLAGS` into `${WORKDIR}/.config`, appends
  `${UNPACKDIR}/defconfig`, then
  `merge_config.sh -O ${B} -m ${WORKDIR}/.config $(find_cfgs(d))` where cml1's
  `find_cfgs` returns **every `*.cfg` in `SRC_URI`**, and finally
  `olddefconfig`. ⇒ a config fragment is just a `.cfg` added by a bbappend
  (`fw-swupdate/recipes-support/swupdate/`), and `-m` means the last file
  wins. `[wrynose]`
- The recipe's anonymous python computes `DEPENDS` from the **merged** config
  text, and its fragment regex is `^(?:# )?(CONFIG_[a-zA-Z0-9_]*)[= ].*\n?` ⇒
  `# CONFIG_FOO is not set` really does cancel a base `CONFIG_FOO=y` *and* the
  `DEPENDS` it pulled in. `[wrynose]`
- Unconditional `DEPENDS += "libconfig zlib libubootenv json-c"` +
  `kern-tools-native`, so **`libubootenv` builds even though we will never call
  U-Boot**. Conditional on symbols: `SURICATTA`/`DOWNLOAD`→curl,
  `MTD`/`CFI`/`UBIVOL`→mtd-utils, `LUA`→lua, `SYSTEMD`→systemd,
  `DISKPART`→util-linux+e2fsprogs, `XZ`→xz, `ARCHIVE`→libarchive,
  `REMOTE_HANDLER`→zeromq, `UCFWHANDLER`→libgpiod, `RDIFFHANDLER`→librsync.
  `[wrynose]`
- `swupdate-www`, `-client`, `-ipc`, `-lua`, `-progress`, `-tools`,
  `-tools-hawkbit`, `-usb` are **packages of the `swupdate` recipe**, not
  separate recipes. `SYSTEMD_SERVICE:${PN} = "swupdate.service swupdate.socket"`,
  `wwwdir ?= "/www"`, `RRECOMMENDS:${PN} += "${PN}-ipc"`, IPC sockets default to
  `/tmp/sockinstctrl` + `/tmp/swupdateprog`, `HW_COMPATIBILITY_FILE =
  "/etc/hwrevision"`. `[wrynose]`
- **Silent-drop hazard:** upstream `Kconfig` declares `HAVE_*` as `option env=`
  symbols that SWUpdate's own Makefile probes from the sysroot
  (`HAVE_LIBMTD`, `HAVE_LUA`, `HAVE_LIBSSL`, `HAVE_LIBSYSTEMD`,
  `HAVE_LIBUBOOTENV`, …). Any symbol with an unsatisfied `depends on HAVE_*` is
  dropped by `olddefconfig` **without a message**, so "the fragment was
  accepted" proves nothing — and `${WORKDIR}/.config` is only the *input*
  merge (it still lists `CONFIG_UBOOT=y`), so it is not the evidence either.
  Mitigation: the CI job dumps the merged `${B}/.config` (the bbappend snapshots
  it to `${T}/swupdate-merged-dotconfig`, and `rm_work` never touches `temp`)
  and asserts `SIGNED_IMAGES`/`HASH_VERIFY` are `y` and `UBOOT`/`MTD` are
  not. `[wrynose]`
- `CURL` and `CURL_SSL` are **hidden** (`default n`, no prompt) — reached only
  via `select` from `CHANNEL_CURL`/`CHANNEL_CURL_SSL` (i.e. `DOWNLOAD`,
  `DOWNLOAD_SSL`, `SURICATTA`). Set them through those, never directly.
  `BOOTLOADER_NONE` is `default y`, so the "Default Bootloader Interface" choice
  resolves itself once `UBOOT` is off. `SIGNED_IMAGES` selects `HASH_VERIFY`
  and both depend on an SSL impl (`SSL_IMPL_OPENSSL=y` is in the shipped
  defconfig); `SIGALG_RAWRSA`/`SIGALG_CMS` default `y`. `[wrynose]`
- The layer also ships **bbclasses** (found 2026-09-23, not used by the compile
  gate): `classes-recipe/swupdate.bbclass` (build a compound `.swu` from an
  update-image recipe's `SRC_URI` + `SWUPDATE_IMAGES`), `swupdate-image.bbclass`
  (`inherit` it in an image recipe → `.swu` from the image itself; requires a
  `file://sw-description`), on top of `swupdate-common.bbclass` (`do_swuimage`
  sstate task, `SWUDEPLOYDIR`, `SWUPDATE_SIGNING`/`SWUPDATE_IMAGES_ENCRYPTED`
  ⇒ `cpio-native` + `openssl-native` DEPENDS, and `S = "${UNPACKDIR}"`).
  This is the likely route for step 5 (and it means signing can happen at
  build time, not by hand). `[wrynose]`
- kas mechanics worth knowing before editing these ymls: configs merge
  key-by-key but **list and scalar values are replaced**, not appended
  (`includehandler._internal_dict_merge`) — which is why `target:
  [swupdate]` in the fragment overrides `solar-ctl-image`. `repos.<id>.layers`
  is a **mapping** (layer path -> null | disabled | {prio}), so an including
  config that adds a key ADDS a layer while the rest of the parent entry
  survives. String entries under `header.includes` resolve against the **repo
  root** (file-relative works but draws a warning). `kas build --target X` (or
  `KAS_TARGET=X`) replaces the config's target outright. `[kas]`

### 6.2 Shipped defconfig vs. our fragment

"fragment" = what `fw-swupdate/recipes-support/swupdate/swupdate/solar-ctl.cfg` asks
for. Because `olddefconfig` can silently drop a symbol (6.1), the table
describes intent — the CI job's `.config` dump is the evidence. That dump now
exists (`[ci]`) and confirms every line of intent above survived
`olddefconfig`, plus `SSL_IMPL_OPENSSL=y` (so `SIGNED_IMAGES` is real, not
probed away) and `LUA=y` against **lua 5.5.0** in oe-core, which was a
worth-worrying-about unknown.

| Symbol                             | shipped   | fragment    | Note                                               |
| ---------------------------------- | --------- | ----------- | -------------------------------------------------- |
| `CONFIG_UBOOT`                     | **y**     | **off**     | no U-Boot; `BOOTLOADER_NONE` is default-y anyway   |
| `CONFIG_MTD` / `CONFIG_CFI`        | **y**     | **off**     | SD card, no flash; also drops mtd-utils DEPENDS    |
| `HASH_VERIFY` / `SIGNED_IMAGES`    | n         | **y**       | signing gate — see 6.4                             |
| `CONFIG_SYSTEMD`                   | n         | **y**       | distro is `INIT_MANAGER=systemd`                   |
| `HANDLER_IN_LUA`                   | n         | n           | slot-switch in Lua vs. shell: still open           |
| `LUA` / `LUASCRIPTHANDLER`         | y         | y           | keep (scripts)                                     |
| `SHELLSCRIPTHANDLER`               | y         | y           | cheap fallback for the same job                    |
| `SCRIPTS`                          | y         | y           |                                                    |
| `RAW`                              | y         | y           | writes `device=` literally                         |
| `WEBSERVER` / `MONGOOSE(SSL)`      | y         | y           | stage 1 delivery; `MONGOOSESSL` is web TLS only    |
| `HW_COMPATIBILITY`                 | y         | y           | ⇒ `/etc/hwrevision` is **mandatory**               |
| `SURICATTA` (+ `CURL`/`CURL_SSL`)  | n         | n (later)   | **this is the Hawkbit client**; `CURL*` are hidden |
| `DISKPART` / `JSON` / `ARCHIVE`    | y/n       | n           | not needed to write squashfs to a partition        |
| `CONFIG_XZ`                        | n         | n           | for xz payloads *inside* the `.swu`, not the roots |
| `UPDATE_STATE_CHOICE_BOOTLOADER`   | **y**     | **y**       | not optional - see 6.3                             |

### 6.3 Things SWUpdate will *not* do for us

- **No RPi bootloader interface.** Bootloader backends are
  `none`/`ebg`/`uboot`/`grub`/`cboot`; there is no `sysboot`/tryboot handler.
  → set `bootloader_transaction_marker = false;` and
  `bootloader_state_marker = false;` and do the switch in a script/preinstall.
- **Update state cannot be stored "in the bootloader", and the Kconfig will
  not warn you.** The "Update Status Storage" choice (`bootloader/Kconfig`) has
  exactly **one** option, `UPDATE_STATE_CHOICE_BOOTLOADER`, and it depends on
  `BOOTLOADER_EBG || UBOOT || BOOTLOADER_NONE || BOOTLOADER_GRUB` — so picking
  `BOOTLOADER_NONE` *forces* it on (a choice with one visible option is not
  optional), which is what the merged config shows:
  `CONFIG_UPDATE_STATE_CHOICE_BOOTLOADER=y` + `UPDATE_STATE_BOOTLOADER="ustate"`
  `[wrynose]` `[ci]`. There is **no file-based alternative to select**.
  Worse, `bootloader/none.c` implements `env_get`/`env_set` on a
  `static struct dict environment` — a **process-local RAM dict** that returns
  0 (success) and evaporates at reboot `[wrynose]`. So `ustate`,
  `swupdate-env`, `-e setenv` and any `set_bootloader_state`/`bootstate`
  property silently *succeed into the void*: no error, nothing persists across
  a reboot. Consequence: **any** boot-state/trial/confirm/bootcount bookkeeping
  is ours to persist (a file under `/data`), and nothing in `sw-description`
  may use `bootstate=`/`set_bootloader_state`/`swupdate-env`. `[wrynose]` (If we
  ever decide that is too painful, §6.6 records a no-new-code escape hatch.)
- **No `dual_copy` property** (that is RAUC vocabulary). Standby-slot
  selection is ours: two `images:` entries +
  `swupdate -i x.swu --select stable,copy-B`.
- `raw` handler opens `img->device` as a **literal path** — use
  `/dev/disk/by-label/rootfs-b`, not `PARTUUID=`/`LABEL=` guesses, or use
  `rawfile` (`device=` + `filesystem=` + `properties={atomic-install="true";}`).
- `installed-directly = true` means **zero-copy streaming into the device**.
  On 512 MB RAM this is **mandatory**: without it SWUpdate extracts into
  tmpfs `/tmp` first and our ~350 MB slot does not fit twice.
- `install-if-hash-different` exists — use it to make re-applying the same
  image a no-op.
- `swupdate.service` + `swupdate.socket` are enabled by *mere installation*;
  IPC at `/tmp/sockinstctrl` + `/tmp/swupdateprog`. Decide whether the daemon
  runs always or socket-activated on demand.

### 6.4 Signing — gate, not a nice-to-have

This repo is public and the web UI binds a TCP port. An unsigned OTA endpoint
is remote root. Before the web UI is reachable from any network:
`CONFIG_SIGNED_IMAGES=y` + `CONFIG_HASH_VERIFY=y`, `swupdate-privkey.pem` kept
off-repo, `--setkey`/`-k <pubkey.pem>` baked into the image, and the
`sha256.hash` + `signature.asn1` checked on every install.

The compile gate confirms the SSL side is not probed away: the merged config
keeps `CONFIG_SSL_IMPL_OPENSSL=y` (GPGME/mbedTLS/WOLFSSL off) alongside
`CONFIG_SIGNED_IMAGES=y` + `CONFIG_HASH_VERIFY=y`, so the `HAVE_LIBSSL`-style
drop hazard did **not** bite here. `[ci]`

### 6.5 Rollback without a boot counter

`tryboot` is consumed by the firmware *before* the kernel runs, so **nothing
decrements a counter if the kernel hangs early**. Mitigations, in order of
cheapness:

1. Put `slot=A`/`slot=B` in each slot's `cmdline`, and have the app/systemd
   unit mark success explicitly (write to `/data/overlay/success`); a unit
   that never sees its own slot token ⇒ do not commit.
2. `panic=N` on the kernel command line so a panic reboots instead of
   hanging — with `tryboot` already cleared by the firmware that reboot goes
   back to the *working* slot. This is the actual fallback that buys us
   safety, and it needs no counter at all.
3. Optional: BCM2835 watchdog (`bcm2835_wdt`) as a hardware panic-path reset.

### 6.6 Could we write an RPi bootloader backend? (analysed, then deferred)

The design-review question was: rather than scripting the slot switch, give
SWUpdate a real `rpi` backend so `ustate`, `bootstate=` and `swupdate-env`
finally mean something. **Answer: it is feasible and it is a documented
extension point — but it would be ~90 % cosmetic, so we defer it.**
Everything below was read first-hand from `sbabic/swupdate` tag `2026.05.1`
(what `meta-swupdate` builds for us) via the GitHub contents API, so every
identifier is copied, not remembered. `[wrynose]`

**The interface is four functions, keyed by a string, not an enum.**
`include/bootloader.h` has `#define BOOTLOADER_NONE "none"` (likewise `ebg`,
`grub`, `uboot`, `cboot`) and
`typedef struct { int (*env_set)(const char *, const char *); int
(*env_unset)(const char *); char *(*env_get)(const char *); int
(*apply_list)(const char *); } bootloader;` plus
`register_bootloader(const char *name, bootloader *bl)`, `set_bootloader`,
`get_bootloader`, `is_bootloader`, `print_registered_bootloaders`, and a
`load_symbol`/`dlsym` macro. `core/bootloader.c` is 78 lines: a realloc-grown
`available` table, and `set_bootloader()` **skips entries whose `funcs` is
NULL** — so registering-then-failing-to-load-a-lib makes the backend
unselectable rather than degrading it (the U-Boot/EBG runtime-lib pattern).

Adding a backend is **four patch points**, all documented in
`doc/source/bootloader_interface.rst` (its "trunk" example):

| #  | File                   | Hook in                                                  |
| -- | ---------------------- | -------------------------------------------------------- |
| 1  | `bootloader/rpi.c`     | new backend: static `bootloader` struct + `constructor`  |
| 2  | `include/bootloader.h` | `#define BOOTLOADER_RPI "rpi"` (one central name)        |
| 3  | `bootloader/Kconfig`   | `BOOTLOADER_RPI` + a `BOOTLOADER_DEFAULT_RPI` choice     |
| 4  | `bootloader/Makefile`  | `obj-$(CONFIG_BOOTLOADER_RPI) += rpi.o`                  |

Row 1 is the whole job: fill `.env_get` / `.env_set` / `.env_unset` /
`.apply_list` and register them from a `__attribute__((constructor))` function.
`Makefile.flags` gains `LDLIBS += dl` *only* if the backend dlopens an env
library (ours would not). The docs additionally ask for the name in
`suricatta.bootloader.bootloaders` in `suricatta/suricatta.lua`.

Runtime selection is `-B rpi` or `globals { bootloader = "rpi"; }`.
`bootloader/Makefile` is an explicit `obj-$(CONFIG_…)` list (no wildcard) plus
an `ifeq` cascade that force-adds `none.o` only when no other backend is
selected. Note the upstream **doc list is stale**: `bootloader_interface.rst`
enumerates RAM/EBG/U-Boot/GRUB and omits `cboot`, which the header, the
Makefile and the Kconfig all have — follow the code, not that page.

**Why it would buy little.**

- The interface is a **key/value environment, and the Pi has no such thing**.
  Boot choice on this board is *which file the firmware reads* plus a one-shot
  flag; there is no `tryboot=stable` variable to write.
- `env_get("tryboot")` could not return firmware truth: the flag is consumed
  pre-kernel, and the kernel driver exposes only `RPI_FIRMWARE_SET_REBOOT_FLAGS`
  — no getter. A backend would be reporting its own bookkeeping. `[inferred]`
- `env_set` is **per variable**, so every state transition becomes a **write to
  FAT on p1** — exactly what §4 forbids ("writes must stay rare") and what §9
  calls a truck-roll risk, because that FAT also holds `start.elf`.
- It fixes none of the actual hard parts (streaming into the standby slot,
  commit-without-brick, absence of a boot counter).

**The shortcut that really works — and is better than it first looks.**
`CONFIG_BOOTLOADER_GRUB` is already a **file-backed** env: `bootloader/grub.h`
has `GRUBENV_SIZE 1024`, `GRUBENV_HEADER "# GRUB Environment Block\n"`, and —
the part worth knowing — the path is a **Kconfig string**
(`config GRUBENV_PATH`, default `/boot/efi/EFI/BOOT/grub/grubenv`, used via
`#ifdef CONFIG_GRUBENV_PATH`), so **no C patch is needed to relocate it**.
`CONFIG_BOOTLOADER_GRUB=y` + `CONFIG_GRUBENV_PATH="/data/ustate/grubenv"` makes
`ustate`, `swupdate-env`, `bootstate=` and `set_bootloader_state` persist, with
zero new code. Costs:

- the file must exist and be **exactly 1024 bytes** starting with that header —
  `grubenv_open()` errors on any other size, so the image pre-creates it;
- writes go to `grubenv.new` then `rename()`, so the directory must be on the
  same filesystem — put it on ext4 `/data`, never on p1;
- `BOOTLOADER_GRUB` has no `depends on HAVE_*`, so `olddefconfig` will **not**
  drop it — and because the `choice "Default Bootloader Interface"` has no
  `default` statement, making GRUB visible **silently flips the default backend
  off `none`** (GRUB precedes NONE in the choice). Pin it explicitly with
  `CONFIG_BOOTLOADER_DEFAULT_NONE=y` if that is not what you want;
- it **lies about the board**: logs and status will say "grub" on a machine
  that has never seen GRUB.

> **Do not read "GRUB" as a bootloader here.** `start.elf` on BCM2835 cannot
> load GRUB, and `BOOTLOADER_GRUB` does **not** switch slots — it only backs the
> key/value env with a file so those state calls persist. It is an optional
> stage-3 escape hatch for *persistence*, nothing more. Plan of record is
> unchanged: `BOOTLOADER_NONE` plus boot-state bookkeeping in `/data` (§6.5).
> "No new C" is the only sense in which it is easier than a backend.

**Decision: defer the backend.** If all we want is persistence, the GRUB-env
trick beats ~300 lines of C plus a 4-file patch we would have to carry forever.
If bench test 2 passes and the firmware owns A/B (Tier 2), a backend becomes
actively pointless. **Trigger to revisit: stage 3**, when `SURICATTA`/Hawkbit
wants real boot-state reporting — and even then, re-read this section first.

### 6.7 What about a U-Boot A/B like `rboussel/SWUpdate-rpi-demo`?

Read the repo for what it proves, not as a recipe. Checked first-hand on
GitHub: last commit 2016-08-26, a **Buildroot** external tree, Shell.
`[reported]`

| What the demo does                                                                                                         | Does it transfer here?                                                                                                                     |
|----------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------|
| `config.txt` sets `kernel=u-boot.bin` (u-boot masquerades as the kernel)                                                   | Yes — and `RPI_USE_U_BOOT=1` emits exactly this on wrynose `[wrynose]`                                                                     |
| p2 = kernel+rootfs-1, p3 = kernel+rootfs-2 (**per-slot kernel**)                                                           | The one genuinely valuable idea; it removes the §9 hazard                                                                                  |
| hand-rolled hush script: `which_fs_part`, `test_count 3`, `setexpr part ${part} ^ 1`, `saveenv`, custom `update_verif`     | Still ours to write — stock `boot.cmd.in` has no A/B, no bootcount `[wrynose]`                                                             |
| `overlay/etc/fw_env.config`, pre-baked `uboot.env(.img)`, `0001-fix-config-file-loading-in-env-library.patch`              | libubootenv friction is real, and 2016-era                                                                                                 |
| DTB `bcm2708-rpi-b-plus.dtb`, `bootz`, `ext2load` of an ext2 root                                                          | No — Pi 1/B+ era, not Zero W; our slots are squashfs, not ext2                                                                             |
| its script hand-rolls no `bootcount`/`altbootcmd`/`distro_bootcmd`                                                         | **Misleading yardstick**: mainline had them *before* the demo (`bootcount_env` 2013-11, `config_distro_bootcmd.h` 2014-08) `[master]`      |

That last row corrects a note I wrote earlier from memory, and the claim is
documented rather than folklore. The A/B-shaped variables are specified in the
**Boot Count Limit** doc, `doc/api/bootcount.rst`: `bootcount` starts at 1,
increments each reboot while `upgrade_available` is non-zero, and when it
exceeds `bootlimit` the firmware runs `altbootcmd` instead of `bootcmd` — which
is the retry-and-fallback mechanism the demo hand-wrote. `doc/develop/distro.rst`
documents `distro_bootcmd`/`$bootcmd` (`config_distro_bootcmd.h`). Two traps for
the next reader: there is **no** `doc/README.bootcount` on master (it moved to
`doc/api/bootcount.rst`), and `doc/README.autoboot` does **not** mention
`altbootcmd` — it defers to the main README. `doc/usage/cmd/bootmeth.rst` also
covers `altbootcmd`, but in bootstd/bootmeth terms. Upstream history puts
`distro_bootcmd` at 2014-08-09 (`2a43201a13`, "config: introduce a generic
$bootcmd"), its doc at 2015-01-30, and the bootcount env backend at 2013-11-11
(`eda0ba38a8`) — all years before the demo. `[master]`

Mainline even ships the exact shape we would want: `CONFIG_BOOTCOUNT_ALTBOOTCMD`
is a default-env variable (`include/env_default.h`), and in-tree defconfigs use
it for A/B rollback — `configs/lxr2_defconfig` sets it to `"run swupdate"`, and
`configs/smegw01_defconfig` flips `mmcpart`/`mmcpart_committed` to the other
copy and resets `bootcount` `[master]`. So a u-boot A/B is **glue around
features that already exist**, which means the demo *under-sells* u-boot: a 2026
script is much shorter than that repo suggests. It is still only evidence that
the per-slot-kernel layout works — which `[bench]` test 1c (`kernel=` stanzas
per slot) is designed to give us **without** a second bootloader.

The remaining cost of U-Boot is a second stage (`u-boot.bin` as `kernel.img`)
that we **never OTA**. The GPIO14/15 “fight” is not a pin mux law: keep
`ENABLE_UART=1`, isolate MAX485 DE/RE at reset, `bootdelay=-2` in production.
Kernel overlay `solar-rs485` takes the pins after `bootz`.

**Decision (2026-09-24, after bench 1c + MAX485 isolation):** plan of record
is **per-slot kernel via U-Boot** (§5). Condition (a) is true (`kernel=` not
honoured). Condition (b) was the wrong question — we do not give up RS485; we
keep the transceiver quiet until Linux.

---

## 7. Hawkbit later

Hawkbit is **a configuration choice, not a protocol to implement**: SWUpdate
is the client, `CONFIG_SURICATTA=y` (which pulls
`SURICATTA_HAWKBIT`, `default y`) plus `CURL`/`CURL_SSL`. Then the device
polls the Hawkbit DDI, and the web UI becomes the local/lab fallback. Needs a
Hawkbit (or Hawkbit-compatible) server — that is the real cost, not the
device-side code.

---

## 8. Bench protocol (gate the design)

Nothing below is decidable from source. Every question is about what the
**BCM2835 `start.elf` we actually ship** does, and that blob is not in this
repo. Tests are ordered by cost: 0 is free, 1–3 need a reboot each, 4 is a
download, 5 waits on a build.

**Before starting:** serial console attached **and captured to a file**
(`picocom … | tee bench-serial.log`), a card reader and a known-good
`.wic.bz2` at hand, and a power socket you can reach. No test touches
`bootcode.bin`/`start.elf` (§9). Expect at least one pull-the-power.

Two properties make this safe, and both shape the protocol:

- `tryboot` is **one-shot** and consumed pre-kernel, so a wrong answer reboots
  back to `config.txt` by itself. No test here can brick the board.
- Consequently **nothing can verify tryboot arming before the reboot.** The
  evidence is always `/proc/cmdline` + the serial log *afterwards*.

`BOOT` below means the vfat mount point, which `ota-probe.sh` prints (do not
assume `/boot`). Record every run in `fw/docs/bench-<date>.md` and paste the
decision into the matrix at the end.

> **Status 2026-09-24 (retested same day):** 1a PASS / 1b FAIL / 1c FAIL /
> 2 PASS / 3 PASS / 4 PASS. Sheet: `fw/docs/bench-2026-09-24.md`. Test 5 waits
> on the §10 step-3 image.

### Arming tryboot

```sh
echo '0 tryboot' > /run/systemd/reboot-param && systemctl reboot
```

needs **systemd ≥ 254**; test 0 prints the version. If it is older, the
userspace route is the same mailbox property the kernel driver issues
(`RPI_FIRMWARE_SET_REBOOT_FLAGS`) — read the tag ID out of
`include/linux/raspberrypi/mailbox.h` on a build host instead of guessing it,
and wrap it the way `rs485ctl` wraps its ioctl. The probe says whether
`/dev/vcio` exists.

### Test 0 — baseline (free, no reboot)

**Purpose:** capture what the board reports today, and read out the
preconditions tests 1–4 depend on. **Do:**

```sh
scp fw/tools/ota-probe.sh root@<board>:/tmp/ && ssh root@<board> 'sh /tmp/ota-probe.sh'
```

**Record:** the whole transcript, in-repo. **Read out:** systemd version (the
arming route), `CONFIG_BLK_DEV_INITRD` (initramfs fallback available),
`CONFIG_SQUASHFS_XZ`/`CONFIG_OVERLAY_FS` (§10 step 3 ready?), p1 mounted `ro`
or `rw`, SD size, `/chosen/bootloader/{version,partnum}` presence, `/dev/vcio`.
**Settles:** which of the fallbacks below are even available. **Flips:** nothing
by itself.

### Test 1 — is `tryboot.txt` honoured, and can it change the cmdline?

> **RESULT:** 1a PASS (11 s vs 6 s), 1b FAIL (`cmdline=` ignored), 1c FAIL
> (`kernel=` ignored). Classic Tier 1 is dead — see §5.

This is the Tier 1 load-bearing question, in three parts. Frame honestly:
whether `cmdline=`/`kernel=` exist **and are honoured** by legacy `start.elf`
is exactly what is unknown — do not assume either.

**1a — was the file consulted at all?** `boot_delay=5` is a harmless knob whose
effect (a 5 s stall before kernel output) is visible on serial, unlike an
unknown key, which the firmware ignores with at most a warning.

```sh
cp "$BOOT/config.txt" "$BOOT/tryboot.txt"
printf 'boot_delay=5\n' >> "$BOOT/tryboot.txt"; sync
echo '0 tryboot' > /run/systemd/reboot-param && systemctl reboot
```

**1b — is `cmdline=` honoured?** Append to the same `tryboot.txt` the *full*
command line (it replaces `cmdline.txt` if honoured at all) plus a marker:

```sh
printf 'cmdline=%s probe=tryboot\n' "$(cat "$BOOT/cmdline.txt")" >> "$BOOT/tryboot.txt"; sync
echo '0 tryboot' > /run/systemd/reboot-param && systemctl reboot
cat /proc/cmdline        # want to see probe=tryboot
```

Three distinguishable outcomes, which is the point: token present → honoured;
no token but the 5 s stall and a serial `Unknown parameter` → **file was read,
key is not supported** (a different and much more useful answer than silence);
no stall → `tryboot.txt` is not being read at all. If `cmdline=` *is* honoured
but `$(cat cmdline.txt)` came out empty, the board will not boot — power-cycle,
it self-heals.

**1c — is `kernel=` honoured?** Negative test: a name that cannot exist.

```sh
cp "$BOOT/kernel.img" "$BOOT/kernel_probe.img"
printf 'kernel=__probe_missing__.img\n' >> "$BOOT/tryboot.txt"; sync   # arm + reboot
```

Fails to boot only when armed (serial complains / blink code) → `kernel=` is
honoured. Boots normally → either ignored *or* the firmware silently fell back
to `kernel.img`; disambiguating those needs another afternoon and changes
nothing, because **both answers mean per-slot kernel images are off the table**.
That is why the ambiguity is allowed to stand.

**Record:** serial log per boot, `/proc/cmdline`, which of 1a/1b/1c held.
**Settles:** Tier 1 as drawn (per-slot `cmdline=` + `kernel=` stanzas).
**Flips:** 1b fails → Tier 1-alt (§5 does not define it yet; see below).
1a fails → `tryboot` is dead on this firmware and Tier 2 is the only
firmware-owned option.

> **Tier 1-alt (keep in your pocket).** If the firmware will not vary the
> command line per boot, the fallback is *not* "SWUpdate rewrites
> `cmdline.txt`": that file is needed by **every** boot, so corrupting it is a
> truck roll. Better: a small **initramfs** whose init reads the slot marker on
> `/data` and `switch_root`s into `rootfs-a`/`rootfs-b` by ext4 label. Costs
> `CONFIG_BLK_DEV_INITRD` (test 0) a few hundred KB, needs **zero** firmware
> cooperation, and a garbage marker can be made to fall back to A.

### Test 2 — does legacy `start.elf` read `autoboot.txt`?

> **RESULT: YES.** `partition` 1 (unarmed `[all]=1`) → 9 (armed `[tryboot]=9`);
> bogus p9 self-heals same boot. Channel is `partition`, not `partnum`. Landing
> on a second FAT is still unproven.

**Purpose:** Tier 2, i.e. firmware-owned A/B. §3.2 says the docs do not restrict
`[tryboot]`/`boot_partition=` to newer models but the file predates the Pi 4
EEPROM flow. Do not extrapolate from `boot_count`/`boot_arg1` (Pi 5) or
`bootvar0` (Pi 4+).

**2a — is the file parsed?** Use a harmless value and read the firmware's own
answer afterwards (`/chosen/bootloader/partnum`, which the probe dumps):

```sh
printf '[all]\nboot_partition=1\n\n[tryboot]\nboot_partition=1\n' > "$BOOT/autoboot.txt"
sync && systemctl reboot          # plain reboot; probe, look for partnum
```

**2b — is the A/B flip honoured?** Now make the tryboot target impossible.
Add `tryboot_a_b=1` to `config.txt` and point `[tryboot]` at a partition that
does not exist:

```sh
printf '[all]\nboot_partition=1\n\n[tryboot]\nboot_partition=9\n' > "$BOOT/autoboot.txt"
sync && echo '0 tryboot' > /run/systemd/reboot-param && systemctl reboot
```

**Record:** whether `partnum` appeared in 2a; whether 2b failed to boot and
recovered by itself on the next reboot. **Settles:** Tier 2. **Flips:** if 2
passes, the **firmware owns A/B** — Tier 2 becomes the plan of record, Tier 1's
FAT text rewrite goes away, and §6.6's backend becomes actively pointless. If 2
fails, Tier 1/1-alt is all there is.

### Test 3 — rewriting p1 (the commit mechanism)

> **RESULT: PASS.** Marker survived >5 s unclean power pull. p1 is rw-by-default
> in this image; "ro by default" is a step-3 property.

**Purpose:** every tier switches slots by rewriting one FAT text file, so this
is not "whether" but **how carefully**. Test the durability of that write.

```sh
mount -o remount,rw "$BOOT"           # test 0 says whether it is ro by default
printf 'probe\n' > "$BOOT/probe-fat.txt"; sync; md5sum "$BOOT/probe-fat.txt"
# now pull the power — do NOT reboot
```

**Record:** mount options before/after, `df` free on p1, whether the marker and
its md5 survived an *unclean* power cut, and how long remount-rw + write +
remount-ro took. **Settles:** the commit procedure — expect ro-by-default,
remount-rw, write, **read back and compare**, `fsync`, remount-ro — and how
long the rw window stays open. **Flips:** nothing in the tier choice (no tier
avoids this write), but a lost marker means `config.txt`/`start.elf` must be
provably unreachable by any update path, not merely "we promise not to".

### Test 4 — SD throughput and RAM headroom

> **RESULT: PASS.** ≈10 MB/s write / ≈21 MB/s read (32 MiB on root; busybox dd
> has no `conv=fsync`). ~350 MB slot ≈ 40–60 s. Root is 135 MB on a 122 GB card
> (resize never ran).

**Purpose:** a ~350 MB slot has to stream over a slow SD card on a board that
cannot double-buffer it (§6.3). `free`/`MemTotal` come from test 0; this adds
speed.

```sh
dd if=/dev/zero of=/data/probe.bin bs=1M count=256 conv=fsync   # drop conv=fsync if rejected
dd if=/data/probe.bin of=/dev/null bs=1M; rm /data/probe.bin
```

**Record:** write and read MiB/s, and whether `/data` mounted rw at all (it is
still ext4-on-the-old-layout today). **Settles:** expected A→B install seconds,
hence whether the update path needs `panic=N`/watchdog cover and whether
`installed-directly=true` streaming keeps up. **Flips:** nothing structural —
too slow means a bigger block size / smaller root, not a new design.

### Test 5 — RO root + `/etc` overlay (needs §10 step 4 built)

> **Not run** — needs the §10 step-4 A/B image (p2/p3 squashfs slots +
> `overlayfs-etc` + `/data`). Sizing is done at wic time, so there is no
> first-boot resize.

**Purpose:** prove squashfs + `overlayfs-etc` + `/data` boot on this board at
all, before any of it is load-bearing. **Do:** flash the milestone image, run
the probe, edit a file under `/etc`, reboot, re-probe; then flip slots *by
hand* (`dd` the squashfs into the other partition, rewrite the select file)
A→B→A.

**Record:** `mount` output showing `/` is an `overlay` on a `squashfs` lower
and `/data/overlay` is rw; the `/etc` edit surviving reboot **and** slot
switch (it should — that is the hazard in §9); hand-flip timings.
**Settles:** whether §5's p4/p5 split and §10 step 3 hold. **Flips:** if the
`/etc` overlay cannot be made to work without `mount -o remount,rw /`, the
`OVERLAYFS_ETC_CREATE_MOUNT_DIRS="0"` note in §9 is the bug to chase.

### Re-evaluation matrix

> From `fw/docs/bench-2026-09-24.md` (firmware 20250801, retested same day):

| Bench outcome                             | Decision it forces                                                | Result |
| ----------------------------------------- | ----------------------------------------------------------------- | ------ |
| 1a + 1b hold                              | Tier 1 stands as the plan of record                               |        |
| 1a holds, 1b fails                        | Tier 1-alt: initramfs slot picker off `/data` (needs INITRD)      |   ✅   |
| 1a fails                                  | `tryboot` dead here → Tier 2 if 2 passes, else manual pick        |        |
| 1c fails to boot only when armed          | per-slot `kernel=` usable → kernel stays slot-coherent            |        |
| 1c boots normally                         | drop per-slot kernels; kernel/modules coherence becomes a §9 risk |   ✅   |
| 2a shows `partnum`, 2b fails to boot      | Tier 2 becomes plan of record; firmware owns A/B                  |  ✅*   |
| 2a shows nothing                          | Tier 2 is dead; Tier 1 / 1-alt only                               |        |
| 3 marker survives a power pull            | commit = write + verify + fsync, p1 ro by default                 |  ✅†   |
| 3 marker lost                             | p1 writes need a second copy of every file it depends on          |        |
| 4 ≥ 5 MB/s write                          | ~350 MB slot in ~70 s: fine, no design change                     |   ✅   |

\* Fired via the `partition` property, not `partnum`, and 2b **self-healed in
the same boot instead of failing** — parsing + section-select + fallback all
work. Not yet shown: booting *lands* on a second FAT partition
(`boot_partition=2` with its own `cmdline.txt`). Tier 2 is live but not yet
plan-of-record; Tier 1-alt is the only fully-proven route today.

† Survived, but today p1 is rw-by-default; "ro by default" is a step-3
milestone-image property, to be re-confirmed there.

Then update §5 (which tier is the plan of record), this matrix, and `.rules`.

---

## 9. Risks / hazards (carried into the implementation)

- `/etc` overlay **upper is shared by slots A and B and survives rollback** —
  a config written by broken release N is still there after rolling back to
  N-1. Keep app config in `/data`, keep `/etc` diffs minimal. Factory reset =
  wipe `/data/overlay/overlay-etc/upper`.
- Never OTA `bootcode.bin`/`start.elf` — a corrupt GPU firmware is a JTAG-and-
  swap-card recovery, i.e. a truck roll. From any **update** path, only ever
  *add* to p1; keep a known good `config.txt`/`tryboot.txt` pair. (The one
  permitted exception is the supervised firmware maintenance op two bullets
  below, where a human with serial and power control is present.)
- **No runtime firmware updater exists in this stack.** The boot files come from
  `rpi-bootfiles`, which pins `raspberrypi/firmware` to a `SRCREV`
  (`RPIFW_DATE ?= "20250801"`, `PV = ${RPIFW_DATE}`) and *deploys* them at build
  time `[wrynose]`. `meta-raspberrypi` has **no** `rpi-update` recipe — no such
  path on the `wrynose` branch and a layer-scoped code search returns zero hits
  `[wrynose]` — so nothing can drag the upstream runtime updater into an image by
  an `IMAGE_INSTALL` typo, and there is nothing to wrap in SWUpdate.
  `rpi-eeprom` does exist, but its `COMPATIBLE_MACHINE` is `raspberrypi4`,
  `raspberrypi4-64` and `raspberrypi5` only; a Zero W has no EEPROM `[wrynose]`.
- **When a firmware fix is genuinely needed it is a maintenance op, not an OTA.**
  This is the gap the bullet above leaves open: p1 is shared by both slots, so no
  slot switch can carry a firmware change. The procedure is to bump
  `rpi-bootfiles`, rebuild, and update p1 as a **supervised bench operation**
  (serial attached, stable power) — never from a SWUpdate path, because rewriting
  `start.elf`/`bootcode.bin` in place is the one action in this design that can
  actually brick. It is the same deliberately non-atomic class as the
  kernel/modules hazard below. Record `bitbake -e rpi-bootfiles` plus its
  `PV`/`SRCREV`/`RPIFW_DATE` before and after, then confirm what really booted
  from `/chosen/bootloader/version` (test 0 and `ota-probe.sh` both dump it);
  `vcgencmd version` would need `libraspberrypi-bin`, which we do not install.
- **Kernel/modules coherence is the one thing A/B does not automatically buy us
  here.** `start.elf` can only load the kernel from the FAT partition
  `[reported]`, so there is **one** `kernel*.img` shared by both slots while
  `/lib/modules/<ver>` lives inside each squashfs slot. Update B with a new
  kernel *and* rewrite the p1 kernel, then roll back to A, and p1 carries the
  new kernel over slot A's older `/lib/modules` `[inferred]`. Per-slot
  `kernel_a.img`/`kernel_b.img` stanzas fix it — which is exactly what
  `[bench]` test 1c probes. **`[bench]` 2026-09-24: 1c came back NOT honoured**
  — so the honest options are to pin the kernel (`SRCREV`) and treat a kernel
  bump as a
  separate, deliberately non-atomic artifact, or accept that a rollback can
  strand an out-of-tree module. Our exposure is `panel-mipi-dbi`
  (`CONFIG_DRM_PANEL_MIPI_DBI=m` in `solar-ctl-slim.cfg`) `[wrynose]`; the RS485
  path survives because `amba-pl011` is `=y` in `bcmrpi_defconfig` `[wrynose]`.
- `OVERLAYFS_ETC_CREATE_MOUNT_DIRS = "0"` or the preinit `mount -o remount,rw /`
  fails forever against squashfs.
- 512 MB RAM: no double-copy installs (see 6.3), and `mksquashfs -b 262144`
  style tuning is a build-host concern, not a target one.
- SD cards: ext4 journal on `/data` is our choice, but keep FAT writes rare.
- **No `bootstate=` / `set_bootloader_state` / `swupdate-env` anywhere**: with
  `BOOTLOADER_NONE` the "bootloader environment" is a RAM dict, so those calls
  succeed and persist nothing (see 6.3; 6.6 has the one no-new-code way out).
  Boot state lives in `/data`.
- Project rules still bind the implementation: `S = "${UNPACKDIR}"`; no line
  starting with `}` inside recipe functions; `IMAGE_BOOT_FILES` /
  `RPI_KERNEL_DEVICETREE_OVERLAYS` set in **kas `local_conf_header`**, not a
  kernel bbappend; never `rm_work_and_downloads`; GitHub mirrors for layers;
  no real WiFi credentials in committed files.

---

## 10. Implementation order

1. **`bitbake swupdate` on musl first.** Everything else is gated on this;
   upstream has no musl patches for it, so this was the real unknown.
   **DONE 2026-09-23 — it builds.** `fw/kas-swupdate.yml` (adds `meta-swupdate`,
   target = `swupdate` only) + the `fw-swupdate` layer (kconfig fragment, kept
   out of every image) + the `swupdate` CI job, which dumps the merged
   `.config` and fails if signing was silently dropped. Result:
   `swupdate_2026.05.1.bb` built on musl with lua 5.5.0, and the dumped config
   shows `SIGNED_IMAGES`/`HASH_VERIFY`/`BOOTLOADER_NONE`/`SYSTEMD` = y and
   `UBOOT`/`MTD`/`SURICATTA` off, as intended. `[ci]` The `wic-native -c fetch`
   check from §2.1 also **PASSED 2026-09-24** `[bench]` — nothing left open in
   step 1.
2. Bench tests 0–4 **DONE** 2026-09-24 (`fw/docs/bench-2026-09-24.md`). Classic
   Tier 1 is dead (`cmdline=`/`kernel=` ignored). Plan of record is **U-Boot +
   per-slot kernel** (§5).
3. **U-Boot smoke image — DONE 2026-09-25 `[bench]`.** One FAT + one ext4
   root, no A/B: WiFi firmware (in the recipe) plus exactly one new variable,
   `RPI_USE_U_BOOT=1`. Evidence (tee'd serial, `build/smoke-*.log`):
   `U-Boot 2026.01 (Jan 05 2026)` banner → `Hit any key to stop autoboot: 2`
   runs to zero unattended → `bootflow scan` picks the stock `boot.scr` →
   `## Booting kernel from Legacy Image` (uImage, checksum OK) → kernel in
   ~6 s, shell in ~40 s. Prompt reachable (stock `U-Boot>`, `bootdelay=2`).
   WiFi works end-to-end: `wlan0` up, WPA associated, DHCP `192.168.0.53/24`
   (needed `kernel-module-brcmfmac-cyw` — see `.rules`). One autoboot was
   interrupted by a stray byte in the 2 s window (open picocom?); the board
   sat at `U-Boot>` — `boot` recovered it, nothing lost. Root sizing,
   `bootdelay=-2` and the custom `boot.cmd` land with the step-4 wks.
4. A/B kickstart `fw/files/wic/solar-ctl-ab.wks.in`: p1 vfat (GPU + `u-boot.bin`
   + `slot-a/`/`slot-b/` kernels) + p2/p3 **squashfs-xz** `rootfs-a`/`rootfs-b`
   + p4 ext4 `/data`. Custom `boot.cmd` with `bootcount`/`altbootcmd`.
   `overlayfs-etc` with `CREATE_MOUNT_DIRS="0"`.
5. SWUpdate in the image (`CONFIG_UBOOT=y`); bench A→B→A. Env on `/data`, never
   the RAM `BOOTLOADER_NONE` dict. `installed-directly=true`. Never OTA p1.
6. Web UI **after** signing is switched on; Hawkbit (`SURICATTA`) last.
