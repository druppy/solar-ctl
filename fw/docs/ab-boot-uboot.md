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
| p1 | vfat | GPU boot files (`bootcode.bin`, `start.elf`), **`u-boot.bin` deployed as `kernel.img`**, `boot.scr`, `uboot.env`, DTB + overlays, factory `uImage` fallback | **Never** |
| p2 | squashfs | root slot **A** (read-only) | via `stable,main` |
| p3 | squashfs | root slot **B** (read-only) | via `stable,alt` |
| p4 | ext4 | `/data`: `overlay-etc/upper` (writable /etc), `log` (journal), `cores/slot-{a,b}/uImage` (the per-slot kernels), app state | kernels via same `.swu` |

`/boot` (p1) is mounted **read-only** by our `boot.mount` unit — nothing
mounts it automatically (see traps).

## Boot chain, top to bottom

1. **GPU first stage** reads `config.txt` + dtb + overlays from p1. This is
   identical for both slots — kernels shipped by OTA must stay
   dtb-compatible, because the dtb never updates.
2. `start.elf` loads `kernel.img`, which is **U-Boot** (`RPI_USE_U_BOOT=1`).
   UART stays enabled (U-Boot + no UART is a build error, and we want the
   serial bench console anyway). Production `boot.cmd` sets `bootdelay=-2`
   (prompt unreachable); the bench build keeps a 2 s window.
3. **U-Boot** reads its environment from `uboot.env` on p1 (via the same
   16 KiB blob `fw_setenv` uses), then runs `boot.scr`:
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
mount -o remount,rw /boot
fw_setenv slot b
mount -o remount,ro /boot
reboot
```

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
`/data/cores` dirs, holds `/boot` writable only during the run, and forwards
to `swupdate`. `-n` defers the reboot; the reboot itself is done by our
`solar-swupdate-progress` service when the install reports SUCCESS —
verified against 2026.05.1: **swupdate never reboots by itself**.

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
  libubootenv writing the same `uboot.env` `fw_setenv` uses.

Delivery proven on bench: URL install (`-d "-u https://..."`) and local
file install, auto-reboot B→A and deferred-reboot A→B.

---

## The traps (each one cost a bench session)

- **`uboot.env` must be shipped, pre-built, complete.** libubootenv cannot
  create it, and a blob containing *only* `slot` is worse than none: a
  valid-CRC environment is treated as COMPLETE, so `bootcmd`/`bootdelay`
  vanish and the board boots into silence. Our recipe merges the real U-Boot
  default environment + `slot=a` into the 16 KiB blob
  (`solar-uboot-env`, CRC32 over the env text, NUL pad). Recovery from a bad
  env at a U-Boot prompt: `env default -a` + `env save`.
- **Nothing mounts `/boot`**: the wrynose imager writes no fstab line for
  the wks `/boot` mountpoint. Without our `boot.mount`, `fw_printenv` dies
  ("Cannot initialize environment") and `solar-update`'s remount fails.
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
