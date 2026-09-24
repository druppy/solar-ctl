# A/B OTA updates for solar-ctl (SWUpdate)

**Status: design record + compile gate PASSED; nothing in an image yet.**
Branch `swupdate_setup`.
The gate question ("does SWUpdate build against our musl/wrynose distro?") is
answered: **yes** — `swupdate_2026.05.1.bb` builds on musl in CI with our
fragment applied, all four kconfig assertions holding (`[ci]` run 35926622985,
artifact `swupdate-dotconfig-<merge-sha>`). The next unknowns are the bench
tests (§10 step 2), not the toolchain.
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

| Area     | Decision                                      | Why                                                             |
| -------- | --------------------------------------------- | --------------------------------------------------------------- |
| Root A/B | **squashfs, xz**                              | RO by construction, no fsck, no journal, ~50 % smaller          |
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

---

## 5. Target disk layout

Partition table stays **MSDOS** (GPT has been reported to upset legacy Pi
boot; Azure's `meta-raspberrypi-adu` warns about it). Consequence: **use ext4
fs labels** (`/dev/disk/by-label/…`) rather than `by-partlabel`, which is a
GPT-only artefact.

| #  | Size (draft) | FS       | Label/role            | Mounted                                 |
| -- | ------------ | -------- | --------------------- | --------------------------------------- |
| p1 | 100 MB       | vfat     | `boot`                | `/boot` (RO-ish, firmware's view)       |
| p2 | ~350 MB      | squashfs | `rootfs-a`            | overlay `lowerdir` of `/`               |
| p3 | ~350 MB      | squashfs | `rootfs-b`            | overlay `lowerdir` when B active        |
| p4 | ~450 MB      | ext4     | `overlay`             | `/data/overlay` (`/etc` + `/var` upper) |
| p5 | rest         | ext4     | `data`                | `/data` (app state)                     |

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
`[bench]` test 1 in §8.

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

**Decision: defer the backend.** If all we want is persistence, the GRUB-env
trick beats ~300 lines of C plus a 4-file patch we would have to carry forever.
If bench test 2 passes and the firmware owns A/B (Tier 2), a backend becomes
actively pointless. **Trigger to revisit: stage 3**, when `SURICATTA`/Hawkbit
wants real boot-state reporting — and even then, re-read this section first.

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

### Test 5 — RO root + `/etc` overlay (needs §10 step 3 built)

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

| Bench outcome                             | Decision it forces                                                |
| ----------------------------------------- | ----------------------------------------------------------------- |
| 1a + 1b hold                              | Tier 1 stands as the plan of record                               |
| 1a holds, 1b fails                        | Tier 1-alt: initramfs slot picker off `/data` (needs INITRD)      |
| 1a fails                                  | `tryboot` dead here → Tier 2 if 2 passes, else manual pick        |
| 1c fails to boot only when armed          | per-slot `kernel=` usable → kernel stays slot-coherent            |
| 1c boots normally                         | drop per-slot kernels; kernel/modules coherence becomes a §9 risk |
| 2a shows `partnum`, 2b fails to boot      | Tier 2 becomes plan of record; firmware owns A/B                  |
| 2a shows nothing                          | Tier 2 is dead; Tier 1 / 1-alt only                               |
| 3 marker survives a power pull            | commit = write + verify + fsync, p1 ro by default                 |
| 3 marker lost                             | p1 writes need a second copy of every file it depends on          |
| 4 ≥ 5 MB/s write                          | ~350 MB slot in ~70 s: fine, no design change                     |

Then update §5 (which tier is the plan of record), this matrix, and `.rules`.

---

## 9. Risks / hazards (carried into the implementation)

- `/etc` overlay **upper is shared by slots A and B and survives rollback** —
  a config written by broken release N is still there after rolling back to
  N-1. Keep app config in `/data`, keep `/etc` diffs minimal. Factory reset =
  wipe `/data/overlay/overlay-etc/upper`.
- Never OTA `bootcode.bin`/`start.elf` — a corrupt GPU firmware is a JTAG-and-
  swap-card recovery, i.e. a truck roll. Only ever *add* to p1; keep a known
  good `config.txt`/`tryboot.txt` pair.
- **Kernel/modules coherence is the one thing A/B does not automatically buy us
  here.** `start.elf` can only load the kernel from the FAT partition
  `[reported]`, so there is **one** `kernel*.img` shared by both slots while
  `/lib/modules/<ver>` lives inside each squashfs slot. Update B with a new
  kernel *and* rewrite the p1 kernel, then roll back to A, and p1 carries the
  new kernel over slot A's older `/lib/modules` `[inferred]`. Per-slot
  `kernel_a.img`/`kernel_b.img` stanzas fix it — which is exactly what
  `[bench]` test 1c probes. If §8 test 1c says `kernel=` is not honoured, the
  honest options are to pin the kernel (`SRCREV`) and treat a kernel bump as a
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
   `UBOOT`/`MTD`/`SURICATTA` off, as intended. `[ci]` Not covered: the
   `wic-native -c fetch` check from §2.1 (still open).
2. Bench tests 0–5 on the *current* layout/probe, per the protocol in §8;
   record results there.
3. Read-only root + `overlayfs-etc` + `/var` overlay + `/data` on the
   **existing** two-partition layout (as far as it goes) → milestone: an
   image that boots RO with writable `/etc`.
4. New kickstart `fw/files/wic/solar-ctl-ab.wks.in` (squashfs slots via
   `--source rawcopy`, per-slot `cmdline_*.txt` + `kernel_*.img` in
   `IMAGE_BOOT_FILES`).
5. SWUpdate end-to-end **A→B→A** cycle on the bench, with the tryboot script.
6. Web UI **after** signing is switched on; Hawkbit (`SURICATTA`) last.
