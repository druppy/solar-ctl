# A/B OTA updates for solar-ctl (SWUpdate)

**Status: design record, nothing implemented.** Branch `swupdate_setup`.
This file is the research + design record so the next session does not have
to re-derive any of it. Facts carry a provenance label:

| Label        | Meaning                                                            |
| ------------ | ------------------------------------------------------------------ |
| `[wrynose]`  | Verified against the oe-core/meta-raspberrypi `wrynose` branch      |
| `[master]`   | Verified against upstream `master` (wrynose may lag — re-check)     |
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
   our `.rules` record as Cloudflare-403 (re-confirmed today: the cgit web UI
   returns a Cloudflare challenge). Our builds do produce `wic.bz2`, so smart
   HTTPS git access evidently works where the web UI does not — but it is one
   Cloudflare policy change away from breaking every build. `[bench]`: run
   `bitbake wic-native -c fetch` and if it fails, add a
   `wic_%.bbappend` pointing `SRC_URI` at a GitHub fork we control (same trick
   as the layer mirrors).
3. To read plugin behaviour (`rawcopy`, `bootimg-pcbios`, …) we can no longer
   grep `layers/openembedded-core/scripts/lib/wic`. It is in the **wic repo**,
   and after a build the only local copy is the download mirror, which
   `rm_work` leaves alone:

   ```sh
   git --no-pager -C build/downloads/git2/git.yoctoproject.org.wic.git log --oneline -n 3
   git --no-pager -C build/downloads/git2/git.yoctoproject.org.wic.git show v0.3.1:lib/wic/plugins/source/rawcopy.py
   ```

   The exact path inside the repo (`lib/wic/plugins/source/rawcopy.py`) is
   `[inferred]` from the old oe-core layout + `python_hatchling`; confirm with
   `git ls-tree -r --name-only v0.3.1 | grep -i rawcopy`. `[bench]`

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

### 3.2 `autoboot.txt` (partition-select style) — attractive, unproven here

`[wrynose]` `config_txt/autoboot.adoc`: `boot_partition=<n>` plus
`tryboot_a_b=1` and a `[tryboot]` section, with a six-partition reference
layout and a "commit by rewriting the file" flow. The `[tryboot]` section,
`cmdline=` and `os_prefix` carry **no model restriction** in the docs.
**Undocumented whether the legacy BCM2835 `start.elf` honours `autoboot.txt`**
(the file predates the Pi 4 EEPROM boot flow) → `[bench]` test 2.
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

| Area     | Decision                                    | Why                                                            |
| -------- | ------------------------------------------- | -------------------------------------------------------------- |
| Root A/B | **squashfs, xz**                             | RO by construction, no fsck, no journal, ~50 % smaller          |
| `root=`  | **`/dev/mmcblk0pN`** device nodes            | matches meta-raspberrypi's own `CMDLINE_ROOT_PARTITION` default |
| `/etc`   | OE-core **`overlayfs-etc`** image feature    | upstream, systemd-aware, factory-reset = wipe upper dir         |
| `/var`   | explicit overlay via **`overlayfs.bbclass`**  | systemd never runs OE's initscripts volatile hook               |
| `/data`  | **ext4 with journal**                        | power-loss safety beats write-count worries for app state       |
| p1 FAT   | boot files only, writes must stay **rare**   | FAT has no journal; never keep a status DB there                |

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

---

## 5. Target disk layout

Partition table stays **MSDOS** (GPT has been reported to upset legacy Pi
boot; Azure's `meta-raspberrypi-adu` warns about it). Consequence: **use ext4
fs labels** (`/dev/disk/by-label/…`) rather than `by-partlabel`, which is a
GPT-only artefact.

| #  | Size (draft) | FS       | Label/role            | Mounted                          |
| -- | ------------ | -------- | --------------------- | -------------------------------- |
| p1 | 100 MB       | vfat     | `boot`                | `/boot` (RO-ish, firmware's view) |
| p2 | ~350 MB      | squashfs | `rootfs-a`            | overlay `lowerdir` of `/`         |
| p3 | ~350 MB      | squashfs | `rootfs-b`            | overlay `lowerdir` when B active  |
| p4 | ~450 MB      | ext4     | `overlay`             | `/data/overlay` (`/etc` + `/var` upper) |
| p5 | rest         | ext4     | `data`                | `/data` (app state)               |

(32 GB card assumed; sizes are `[inferred]` starting points, not measured.
Squashfs slots must be **fixed size ≥ payload** — a slot that is 1 KB too
small is a failed install, so leave headroom and assert it in CI.)

### Tier 1 — one FAT, per-slot cmdline (plan of record)

p1 holds `bootcode.bin`, `start.elf`, `config.txt`, `tryboot.txt`,
`kernel*.img`, `*.dtb`, `overlays/`, and **`cmdline_a.txt` /
`cmdline_b.txt`**, with per-slot stanzas in `config.txt`/`tryboot.txt`
selecting `kernel=` + `cmdline=`. SWUpdate writes the *inactive* squashfs
slot, then rewrites one FAT text file to switch. No repartitioning, no
partition-table writes — the SD geometry never changes, which is exactly what
you want when an OTA fails on a box that is unreachable on a roof.

Feasibility of `[cmdline]`/`kernel=` per boot on BCM2835 `start.elf` is
`[bench]` test 1.

### Tier 2 — `autoboot.txt` partition select

`autoboot.txt` with `boot_partition=` + `tryboot_a_b=1`, roots as separate
partitions, commit = rewrite `autoboot.txt`. Only if test 2 shows the legacy
firmware honours it. Nicer semantics (firmware owns "which partition"), costs
a second FAT-ish layout decision.

### Tier 3 — U-Boot + `bootcount`

Explicitly **rejected**: adds a second bootloader, a `distro_feature`, and
a boot chain we would have to debug over one serial line on a board whose only
UART is already contested by RS485.

---

## 6. SWUpdate plan

### 6.1 Layer & recipe

- `meta-swupdate` has a **`wrynose`** branch; `LAYERSERIES_COMPAT = "whinlatter
  wrynose"`, `LAYERDEPENDS = "openembedded-layer"` — which we already carry.
  Add it to `fw/kas-rpi0.yml` as `meta-swupdate` (mirror on GitHub). `[reported]`
- No `PACKAGECONFIG`: SWUpdate is configured by its **own Kconfig**, with our
  fragment merged over `recipes-support/swupdate/swupdate/defconfig` via
  `merge_config.sh`. `[reported]`
- `swupdate-www`, `-client`, `-ipc`, `-lua` are **packages of the `swupdate`
  recipe**, not separate recipes. `[reported]`
- `libubootenv` is an unconditional `DEPENDS` of the recipe — it will build
  even though we will never call U-Boot. `[reported]`

### 6.2 Shipped defconfig vs. what we need

| Symbol                     | shipped     | we need | Note                                        |
| -------------------------- | ----------- | ------- | ------------------------------------------- |
| `CONFIG_UBOOT`             | **y**       | **n**   | no U-Boot; `BOOTLOADER_NONE` currently unset |
| `HANDLER_IN_LUA`           | n           | maybe   | our own slot-switch logic in Lua            |
| `LUA`/`LUASCRIPTHANDLER`   | y           | y       |                                              |
| `SHELLSCRIPTHANDLER`       | y           | y       | cheap fallback for the same job             |
| `SCRIPTS`                  | y           | y       |                                              |
| `RAW`                      | y           | y       | writes `device=` literally                  |
| `WEBSERVER`/`MOONGOOSE(SSL)` | y         | y       | stage 1 delivery                            |
| `HW_COMPATIBILITY`         | y           | y       | ⇒ `/etc/hwrevision` is **mandatory**        |
| `CONFIG_SYSTEMD`           | n           | y?      | decide at implementation                      |
| `SURICATTA`                | n           | later   | **this is the Hawkbit client**              |
| `CURL` / `CURL_SSL`        | n           | later   | required by suricatta                       |
| `HASH_VERIFY`/`SIGNED_IMAGES` | n        | **y**   | signing gate — see 6.4                      |
| `MTD`/`CFI`/`DISKPART`/`JSON`/`ARCHIVE` | y/n | mostly n | drop the MTD/CFI noise for an SD card |

### 6.3 Things SWUpdate will *not* do for us

- **No RPi bootloader interface.** Bootloader backends are
  `none`/`ebg`/`uboot`/`grub`/`cboot`; there is no `sysboot`/tryboot handler.
  → set `bootloader_transaction_marker = false;` and
  `bootloader_state_marker = false;` and do the switch in a script/preinstall.
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

---

## 7. Hawkbit later

Hawkbit is **a configuration choice, not a protocol to implement**: SWUpdate
is the client, `CONFIG_SURICATTA=y` (which pulls
`SURICATTA_HAWKBIT`, `default y`) plus `CURL`/`CURL_SSL`. Then the device
polls the Hawkbit DDI, and the web UI becomes the local/lab fallback. Needs a
Hawkbit (or Hawkbit-compatible) server — that is the real cost, not the
device-side code.

---

## 8. Bench tests (gate the design)

Run `fw/tools/ota-probe.sh` on current image first (read-only), then:

| #  | Question                                                       | Pass criterion                              |
| -- | -------------------------------------------------------------- | ------------------------------------------- |
| 0  | Baseline: what does the board report today?                     | probe output captured in this repo          |
| 1  | Does `start.elf` honour per-slot `kernel=`/`cmdline=` stanzas?  | boots with `root=/dev/mmcblk0p3` from p1    |
| 2  | Does `autoboot.txt` + `tryboot_a_b=1` work on BCM2835?          | slot flips across a reboot                  |
| 3  | `tryboot.txt` as a full alt config + `0 tryboot` reboot          | one-shot observed, then reverts to A        |
| 4  | squashfs root + `overlayfs-etc` + `/var` overlay + `/data` ext4  | boots RO, config survives reboot, A↔B by hand |

Test 1 decides Tier 1; test 2 decides Tier 2. If both fail, the fallback is a
single `cmdline.txt` on p1 that SWUpdate rewrites (still A/B on the squashfs
slots; only "which file the firmware reads" disappears from the design).

---

## 9. Risks / hazards (carried into the implementation)

- `/etc` overlay **upper is shared by slots A and B and survives rollback** —
  a config written by broken release N is still there after rolling back to
  N-1. Keep app config in `/data`, keep `/etc` diffs minimal. Factory reset =
  wipe `/data/overlay/overlay-etc/upper`.
- Never OTA `bootcode.bin`/`start.elf` — a corrupt GPU firmware is a JTAG-and-
  swap-card recovery, i.e. a truck roll. Only ever *add* to p1; keep a known
  good `config.txt`/`tryboot.txt` pair.
- `OVERLAYFS_ETC_CREATE_MOUNT_DIRS = "0"` or the preinit `mount -o remount,rw /`
  fails forever against squashfs.
- 512 MB RAM: no double-copy installs (see 6.3), and `mksquashfs -b 262144`
  style tuning is a build-host concern, not a target one.
- SD cards: ext4 journal on `/data` is our choice, but keep FAT writes rare.
- Project rules still bind the implementation: `S = "${UNPACKDIR}"`; no line
  starting with `}` inside recipe functions; `IMAGE_BOOT_FILES` /
  `RPI_KERNEL_DEVICETREE_OVERLAYS` set in **kas `local_conf_header`**, not a
  kernel bbappend; never `rm_work_and_downloads`; GitHub mirrors for layers;
  no real WiFi credentials in committed files.

---

## 10. Implementation order

1. **`bitbake swupdate` on musl first.** Everything else is gated on this;
   upstream has no musl patches for it, so this is the real unknown. Also
   verify `bitbake wic-native -c fetch` (§2.1).
2. Bench tests 0–4 on the *current* layout/probe; record results here.
3. Read-only root + `overlayfs-etc` + `/var` overlay + `/data` on the
   **existing** two-partition layout (as far as it goes) → milestone: an
   image that boots RO with writable `/etc`.
4. New kickstart `fw/files/wic/solar-ctl-ab.wks.in` (squashfs slots via
   `--source rawcopy`, per-slot `cmdline_*.txt` + `kernel_*.img` in
   `IMAGE_BOOT_FILES`).
5. SWUpdate end-to-end **A→B→A** cycle on the bench, with the tryboot script.
6. Web UI **after** signing is switched on; Hawkbit (`SURICATTA`) last.
