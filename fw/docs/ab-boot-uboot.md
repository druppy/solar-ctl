# U-Boot + A/B boot — the current setup

**What is true today** (as of 2026-10-02). This is the operational summary;
the research, alternatives-matrix and design arguments are in
`swupdate-ota.md` (the research record), and the raw measurements behind it
are in `bench-2026-09-24.md`. If you just need to know *how this board
boots and updates*, this is the file.

---

## Why a second-stage bootloader at all

The Raspberry Pi first stage (`bootcode.bin`/`start.elf`) was measured, not
assumed (bench 2026-09-24): it **ignores** per-boot `kernel=` and `cmdline=`
variants (tests 1b/1c FAIL). `tryboot` can switch which `config.txt`-adjacent
file is read, but cannot move `root=` and the kernel together, which is what
A/B actually requires. Decision (2026-09-25, plan of record): U-Boot is
loaded as if it were the kernel, and **its environment variable
`slot=a|b` is the A/B truth** — one boot script then loads the matching
kernel and appends the matching `root=`.

| Fact | Consequence |
| ------------------------- | ------------------------------------------------------ |
| start.elf only reads p1 | p1 (dtb, overlays, GPU files) is **shared, never OTA'd** |
| kernel + root must flip together | U-Boot env `slot` selects both (kernel on /data, root on p2/p3) |
| 512 MB RAM, SD card | squashfs (xz) read-only roots, `installed-directly` on install |
| no RTC, headless | everything that can go wrong must be diagnosable on serial |

---

## Card layout

| Part | FS | Contents | OTA'd? |
| ----- | ------ | ------------------------------------------------------------------ | ------------ |
| p1 | vfat | GPU boot files (`bootcode.bin`, `start.elf`), **`u-boot.bin` deployed as `kernel.img`**, `boot.scr`, DTB + overlays, factory `uImage` fallback | **Never** |
| — | raw | hidden (no table entry) redundant U-Boot env pair, flush with p1's end: 2 × 16 KiB `[crc32][flags][data]` copies at the offsets from class `solar-ablayout` (2026-10-05; replaced `/boot/uboot.env`) | written only by `fw_setenv`/SWUpdate |
| p2 | squashfs | root slot **A** (read-only) | via `stable,main` |
| p3 | squashfs | root slot **B** (read-only) | via `stable,alt` |
| p4 | ext4 | `/data`: `overlay-etc/upper` (writable /etc), `log` (journal), `cores/slot-{a,b}/uImage` (the per-slot kernels), app state | kernels via same `.swu` |

`/boot` (p1) is mounted **read-only** by our `boot.mount` unit — nothing
mounts it automatically (see traps). Since 2026-10-05 its only Linux-side
consumer is the first-boot kernel seed; slot switches write the raw env
area instead, so p1 is now **never written by Linux at all**.

## Boot chain, top to bottom

1. **GPU first stage** reads `config.txt` + dtb + overlays from p1. This is
   identical for both slots — kernels shipped by OTA must stay
   dtb-compatible, because the dtb never updates.
2. `start.elf` loads `kernel.img`, which is **U-Boot** (`RPI_USE_U_BOOT=1`).
   UART stays enabled (U-Boot + no UART is a build error, and we want the
   serial bench console anyway). Production `boot.cmd` sets `bootdelay=-2`
   (prompt unreachable); the bench build keeps a 2 s window.
3. **U-Boot** reads its environment from the RAW redundant pair in the
   hidden area (`ENV_IS_IN_MMC` + `ENV_REDUNDANT`; both copies carry a CRC,
   the higher `flags` byte wins, so a write torn by a power cut always
   leaves one valid copy), then runs `boot.scr`:
   - `ext4load mmc 0:4 ${kernel_addr_r} /cores/slot-${slot}/uImage`
     — that is **p4** (0:4), the common mistake here is writing 0:3;
   - fallback: the FAT `uImage` on p1 (a factory card before `/data` exists);
   - bootz with `root=/dev/mmcblk0p2` (slot a) or `p3` (slot b),
     `rootfstype=squashfs rootwait`.
4. **Kernel**: squashfs support is **built-in** (`CONFIG_SQUASHFS=y`) —
   there is no initramfs, and `=m` means a VFS panic on first boot.
   `/data` self-heals in the overlayfs-etc preinit (mount → `e2fsck` →
   `mkfs.ext4` as last resort; boot never blocks).
5. **systemd**: `/etc` is an overlay (upper on `/data`), `/var` parts are
   systemd's own tmpfs overlays, journald persists under `/data/log` with
   size caps (SD-wear bound).
6. **First boot ever**: `solar-cores-seed.service` copies `/boot/uImage`
   into `/data/cores/slot-a/` so the OTA-era boot path has a kernel to load.

## The A/B truth, and how to look at it

```sh
cat /proc/cmdline      # which root this boot used (the real answer)
fw_printenv slot       # which root the NEXT boot will use
journalctl -b -1 -u solar-swupdate-progress   # last boot's update story
```

Manual flip (bench-validated):

```sh
fw_setenv slot b    # raw redundant env area — no /boot mount, no rw dance
```

### Power-cut-mid-`fw_setenv` (bench 2026-10-06: PASS)

`fw/tools/env-powercut.sh` drives the redundant pair through every state a
power cut mid-write can leave (log: `fw/tools/powercut-results.tsv`,
gitignored). All PASS — the board always booted with exactly one
consistent `slot` value:

- **Real physical cut** (`arm`: raise the kernel dirty-writeback window,
  `fw_setenv`, human pulls the plug at the printed prompt): the new value
  landed fully — libubootenv fsyncs internally before `fw_setenv`
  returns, so our side of the wire is already power-cut-safe.
- **Synthesized torn promotion** (`torn`, fully self-run): the standby
  copy is promoted completely (slot flipped, flags max+1, fresh CRC) but
  only 19 of its 32 sectors are flushed — highest flags, dead CRC, the
  exact state a cut leaves. Rejected as designed: the board booted the
  untouched active copy. Fixture subtlety: the CRC covers data only and
  our env generations differ *only* in the `slot` byte, so a flip-flop
  promotion is byte-identical to the old standby and a partial flush is
  trivially a complete write (the first run proved the wrong thing with
  a valid CRC); one byte stamped in the NUL padding beyond the flush
  window makes the tail genuinely stale.
- **Active copy dead** (`corrupt`, CRC invalidated): boots the surviving
  copy, and the next `fw_setenv` rewrites the dead copy byte-exactly —
  self-heal verified.

`boot.cmd` additionally re-validates `slot` (empty/garbage → `a`, no
`saveenv`), so even a hypothetically dead pair never blocks the boot.

---

## Updating: one signed `.swu`, two sets

SWUpdate installs **both slots from a single artifact**; the set decides
where:

| Set | Installs |
| ---------- | ------------------------------------ |
| `stable,main` | p2 rootfs + `/data/cores/slot-a/uImage` |
| `stable,alt` | p3 rootfs + `/data/cores/slot-b/uImage` |

`solar-update` (our small wrapper) reads `root=` from `/proc/cmdline`, so
"install" always means *into the slot you did not boot from*, prepares the
`/data/cores` dirs, and forwards to `swupdate`. `-n` defers the reboot; the
reboot itself is done by our `solar-swupdate-progress` service when the
install reports SUCCESS — verified against 2026.05.1: **swupdate never
reboots by itself**.

Non-obvious rules baked into the payload/recipe (each is a runtime failure
mode, not a style preference):

- **Signatures: RSA-4096 raw** — ed25519 is impossible here (streaming
  `EVP_DigestVerify` has no ed25519 support; the same keypair will sign
  U-Boot FIT images later). Key comes from `SOLAR_SWU_PRIVATE_KEY` (content
  or absolute path); no committed fallback, empty = build error.
- The per-slot kernel entry must be `type = "rawfile"` + `path = ...`;
  type `raw` tries to `open()` a nonexistent device and dies streaming.
- `installed-directly = true` is **mandatory on 512 MB** (else extraction
  lands in tmpfs).
- `sha256` / `installed-directly` are **direct entry attributes** — putting
  them in `properties{}` compiles fine and dies at install with
  "Hash not set for ...".
- hardware compatibility is live against `/etc/hwrevision`, which **must be
  two tokens** (`solar-ctl 1.0`); a lone version string fails every install
  with "SW not compatible with hardware".
- the `bootenv { name = "slot"; value = "a|b"; }` entry flips the slot via
  the U-Boot backend, and only **after** every image of the set installed —
  an env-write failure fails the update, never half-flips.
- no scripts in the `.swu` at all (LUA/scripts compiled out) — pure data.
- the same `.swu` also carries `bootenv`-flush ordering guarantees via
  libubootenv writing the same raw env pair `fw_setenv` uses
  (`/etc/fw_env.config` two-line legacy redundant format, rewritten by
  `u-boot_%.bbappend`).

Delivery proven on bench: URL install (`-d "-u https://..."`) and local
file install, auto-reboot B→A and deferred-reboot A→B.

---

## The traps (each one cost a bench session)

- **The env area must be shipped, pre-built, complete — and it is the
  layout's most sacred 32 KiB.** The seed blob must contain the FULL
  default environment plus `slot=a`: a valid-CRC environment is treated as
  COMPLETE, so a slot-only blob silently deletes `bootcmd`/`bootdelay` and
  the board boots into silence (hit 2026-09-28 with the old FAT blob;
  recovery was `env default -a` + `env save` at the U-Boot prompt — which
  still works: with both copies CRC-invalid U-Boot falls back to compiled
  defaults). `solar-uboot-env` writes `u-boot-initial-env` + `slot=a` twice
  with flags 0/1; u-boot and libubootenv agree on the byte layout
  (`[crc32 LE][flags][data]`, crc over data only). Copy0 at the class
  offset, copy1 at +16 KiB — wic `rawcopy --no-table` bakes both at flash
  time; a garbage area is self-healing on first `saveenv` (flags 0 < 1).
- **Nothing mounts `/boot`**: the wrynose imager writes no fstab line for
  the wks `/boot` mountpoint (matters only for the first-boot kernel seed
  now — the env left p1 on 2026-10-05).
- **`CONFIG_SQUASHFS=m` panics** first boot (no initramfs to mount root).
- **boot.scr loads from `mmc 0:4`** — p4. Writing `0:3` points at the
  *other root slot* and fails obscurely.
- **`/etc` overlay upper is shared by both slots and survives rollback**:
  an `/etc` edit made on A is still there when you fall back to B. Factory
  reset = wipe the upper dir; application config belongs in `/data`, not
  `/etc`.
- **OTA never touches p1.** No path updates `start.elf`/`bootcode.bin` (the
  one brick-capable action here) — GPU firmware changes are a supervised
  bench maintenance op. Kernels via OTA must stay compatible with the
  installed dtb/overlays for the same reason.
- **The serial console is the mini-UART**: it eats characters on lines
  >~60 chars. Bench commands stay short; large files move over the LAN
  (wget), never as typed base64.
- A/B is **slot switching, not signing the card layout**: reflashing wipes
  `/data`, which returns the system to "factory seed" state (first-boot
  seed re-copies the kernel).

## Current validation status

| Date | What | Result |
|------------ | --------------------------------------------------------- | ------------ |
| 2026-09-24 | start.elf per-boot control (tests 1a–1c) | 1b/1c FAIL → U-Boot chosen |
| 2026-09-25 | test 5: full A/B chain, A→B→A flip, `/etc` overlay | PASS |
| 2026-09-28 | signed single-`.swu` flow: URL install, auto-reboot, `-n` | PASS (bench) |
| 2026-10-02 | full image w/ display + `SOLAR_SSH_PUBLIC_KEY`; kernel/rootfs OTA to slot B | image booted A; OTA = the current bench step |
| 2026-10-05 | raw redundant U-Boot env in hidden SD area (off p1 FAT): manual A→B→A flips, 2× URL OTA (`stable,main`/`stable,alt`), ext4load of both `/data/cores/slot-*/uImage`, bootenv write via libubootenv | PASS (bench) |
| 2026-10-06 | power-cut-mid-`fw_setenv` on the raw pair (`fw/tools/env-powercut.sh`): real plug-pull, synthesized torn promotion (19/32 sectors, flags max+1), active-copy CRC kill, clean control | PASS (bench): torn/highest-flags-invalid copies rejected, boot always lands on one consistent `slot`; dead copy self-heals on next `fw_setenv` |
