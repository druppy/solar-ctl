# solar-ctl firmware (fw/)

Yocto **kas** build for the **Raspberry Pi Zero W** inverter controller,
on the **wrynose** release. Standalone **openembedded-core** — *no poky* —
with a deliberately minimal image (~120 packages, ~48 MB compressed):
**musl** instead of glibc, **busybox**, **systemd**.

## CI / releases

`.github/workflows/firmware.yml` builds the image with
`kas build fw/kas-rpi0.yml` (plus a throwaway WiFi fragment when the repo
secrets are set):

- **push/PR to main** — build + `solar-ctl-image-<sha>` artifact (`wic.bz2`,
  `bmap`, `manifest`, signed `.swu`, `SHA256SUMS`)
- **tag `v*`** — same build, additionally published as a GitHub Release
- **signing keys** — the build step gets `SOLAR_SWU_PRIVATE_KEY` from the
  repo secret (the public key is derived from it, never stored separately);
  an unset secret **fails the build** - the repo ships no fallback keypair,
  so no build can ever sign with a key someone left in the tree. The job
  also asserts SWUpdate's merged
  kconfig (signing + U-Boot bootenv backend + URL download on;
  web UI/lua/scripts/MTD/SURICATTA off) from the
  `${T}/swupdate-merged-dotconfig` snapshot — kconfig drops symbols
  silently and the file is the only proof of what stuck.
- **caching**: only `build/sstate-cache` (~1.3 GB) is cached —
  `build/downloads` is ~9.5 GB (8.4 GB of it is `git2` bare clones) and
  GitHub's per-repo cache quota is 10 GB, so caching downloads sat at
  9.97 GiB with zero headroom. `restore-keys` fall back to the newest
  `yocto-*` cache when the key rotates, and saves carry a `-<sha>` suffix
  so every push (and same-repo PR) refreshes the cache (sstate is
  content-addressed — a
  stale-but-newer cache still only rebuilds what changed).
- **cold** builds take well over an hour on runners; warm (cache hit)
  builds are minutes. First run is always cold.

Release flow:

```sh
git tag v0.1.0 && git push origin v0.1.0
```

**WiFi in CI:** if the repo secrets `SOLAR_WIFI_SSID`, `SOLAR_WIFI_PSK`
(and optionally `SOLAR_WIFI_COUNTRY`) are set, Actions injects them as a
throwaway kas fragment and the credentials are **baked into the built
image**. This repo is public — artifacts/releases are world-readable, so
only define these secrets for a **dedicated lab/guest SSID**, or leave
them unset (builds then keep the `CHANGEME` placeholders). Set them with:

```sh
gh secret set SOLAR_WIFI_SSID    # then type the value
gh secret set SOLAR_WIFI_PSK     # hex PSK (wpa_passphrase) or passphrase
gh secret set SOLAR_WIFI_COUNTRY # optional, e.g. DK (default GB)
```

## Layout

| Path                                     | Purpose                                                                                       |
| ---------------------------------------- | --------------------------------------------------------------------------------------------- |
| `kas-rpi0.yml`                           | **Build this** — repos (wrynose branch tips, incl. `meta-swupdate`), machine, WiFi + signing-key + SSH-pubkey env, local.conf bits |
| `conf/layer.conf`                        | `fw/` is a small meta layer (`solar-ctl`)                                                     |
| `conf/distro/solar-ctl.conf`             | Our distro: `TCLIBC=musl`, `INIT_MANAGER=systemd`, lean distro features                       |
| `files/wic/solar-ctl-ab.wks`             | A/B disk layout: p1 vfat + p2/p3 squashfs slots + p4 ext4 `/data`                             |
| `classes/solar-swu-signing.bbclass`      | `SOLAR_SWU_*_KEY` resolution (content / path; no fallback; public key derived + cross-checked) → SWUpdate signing |
| `recipes-core/images/solar-ctl-image.bb` | The image (U-Boot A/B layout, SWUpdate agent, RO root + `/etc` overlay)                       |
| `recipes-core/images/solar-ctl-swu.bb`   | Single signed `.swu` (both slots as `stable,main`/`stable,alt` sets)                          |
| `recipes-support/swupdate/`              | bbappend + kconfig fragment: minimal swupdate (signing on, web UI/lua/scripts off)            |
| `recipes-support/solar-swu-agent/`       | On-target glue: `solar-update`, pub key, hwrevision, kernel-seed + progress services          |
| `recipes-bsp/u-boot/`                    | U-Boot ext4 fragment + pre-seeded `uboot.env` blob                                            |
| `recipes-connectivity/solar-wifi/`       | WiFi bring-up: supplicant config + systemd units                                              |
| `recipes-core/dropbear/`                 | bbappend: root-only key-only SSH config                                                       |
| `recipes-core/solar-rootkeys/`           | `/root/.ssh/authorized_keys` baked from `SOLAR_SSH_PUBLIC_KEY` (public half)                 |
| `recipes-kernel/linux/`                  | Kernel config slimming + nv3007/solar-rs485 DT overlays (142×428 panel-mipi-dbi TFT)          |
| `recipes-support/rs485ctl/`              | RS485 RTS direction-control setup tool                                                        |
| `recipes-support/solar-panel-fw/`         | `/lib/firmware/panel-mipi-dbi-spi.bin` — NV3007 init sequence for panel-mipi-dbi             |
| `recipes-apps/inv-ctl/`                  | LVGL UI app built from repo-root `inv_ctl/` (fbdev backend, `inv-ctl.service`)                |
| `docs/swupdate-ota.md`                   | **A/B OTA design record** (U-Boot per-slot kernel, squashfs roots, `/etc` overlay, SWUpdate)  |
| `docs/ab-boot-uboot.md`                  | **Current U-Boot + A/B setup** (layout, boot chain, slot switch, update flow, traps)          |
| `docs/display-nv3007.md`                 | **Display bring-up story** (panel-mipi-dbi traps, firmware blob, GRAM offset, fbcon)          |
| `tools/ota-probe.sh`                     | Read-only on-target probe of boot chain/filesystems (run before OTA work)                     |
| `tools/swu-keygen.sh`                    | Generate the RSA-4096 `.swu`/FIT signing keypair                                              |
| `tools/lvgl-nv3007-to-mipi-dbi.py`       | Generate the panel firmware blob from LVGL's nv3007 driver (re-run on LVGL bumps)             |

SWUpdate's bbappend lives in `fw/` and is therefore parsed by **every** build —
that is only legal because `kas-rpi0.yml` adds `meta-swupdate` (a dangling
`*.bbappend` is a hard bitbake error). Never remove that repo entry without
moving the bbappend out of `fw/` again.

## Peripherals & wiring (40-pin header)

The Zero W's SoC has two UARTs (PL011 `uart0`, mini-UART `uart1`) and
one usable SPI (SPI0), but the header only carries **one UART data
pair (GPIO14/15, physical 8/10)** — the PL011's own default pins
(GPIO30–33) are wired to the onboard WiFi/BT chip inside the Zero W,
not to the header. Plan around that: console and native RS485 share
the same two pins and are switched in `config.txt`; SPI0 and the
remaining GPIOs are free for the display.

```mermaid
flowchart LR
    subgraph SoC [BCM2835]
        MINI[mini-UART uart1 ttyS0];
        PL011[PL011 uart0 ttyAMA0];
        SPI0[SPI0 spi0];
    end
    subgraph H [40-pin header]
        P810["GPIO14/15 P1-08/10"];
        P11["GPIO17 P1-11"];
        PSPI["GPIO7-11 P1-24/19/23/21/26"];
        PGPIO["GPIO25/24/18 P1-22/18/12"];
    end
    MINI --- P810
    PL011 -. "dtoverlay=solar-rs485" .-> P810
    PL011 -. RTS dir .-> P11
    SPI0 --- PSPI
    PGPIO --> TFT[NV3007 TFT]
```

### Debug console (default)

| signal | GPIO | physical pin |
| --- | --- | --- |
| TXD (→ adapter RX) | GPIO14 | P1-08 |
| RXD (← adapter TX) | GPIO15 | P1-10 |
| GND | — | P1-06 |

115200 8N1, mini-UART (`ttyS0`), root auto-login on the lab image.

> **Numbering caveat**: pinouts like pinout.xyz show three schemes per
> pin. We use **BCM GPIO + physical pin** (`P1-nn`) — the kernel/DT
> convention, same as the overlays. The third column (wiringPi, `wPi`)
> reads **15/16 on pins 8/10** — that is legacy library numbering (its
> TXD/RXD are wPi 15/16), not GPIO numbers; ignore it.

### NV3007 2.79" TFT (142×428 SPI, enabled by default)

TZT 2.79" 142×428 SPI display with the NV3007 controller (AliExpress:
"2.79 Inch NV3007 TFT LCD Display Module TZT 142×428 Resolution 8 Pin
SPI Full Color Screen Panel"). It is driven
by the generic in-kernel `panel-mipi-dbi` driver (not `ili9341` — that
one hardcodes 240×320) via our `nv3007` overlay, which is **on** in
`config.txt`. The resolution comes from the overlay's `panel-timing`
node; the controller init sequence comes from a firmware file, see
below. The module's 8-pin header, numbered 1→8 next to the pads:

| # | display pin | RPi signal | GPIO | RPi header | overlay override |
| --- | --- | --- | --- | --- | --- |
| 1 | GND | ground | — | P1-06/09/14/20/25/30/34/39 | — |
| 2 | VDD | 3.3 V | — | 3V3 (P1-01/17) | — |
| 3 | SCI | SPI clock (SCLK) | GPIO11 | P1-23 | — |
| 4 | SDA | SPI MOSI | GPIO10 | P1-19 | — |
| 5 | RES | reset | GPIO24 | P1-18 | `reset_pin=<n>` |
| 6 | DC | data/command | GPIO25 | P1-22 | `dc_pin=<n>` |
| 7 | CS | SPI CE0 | GPIO8 | P1-24 | — |
| 8 | BL | backlight | GPIO18 | P1-12 | `led_pin=<n>` (0 = tie to 3V3) |

Runs at 3.3 V — do not feed 5 V into data lines unless your module
board is explicitly 5 V-tolerant. Backlight is driven from GPIO18 via
`gpio-backlight` (on at boot); wiring BLK straight to 3V3 also works —
the GPIO then just toggles a disconnected pin.

**GRAM offset & SPI speed:** this TZT module's visible window starts at
GRAM x=14; the overlay's `panel-timing` encodes that as
`hback-porch = <14>` (`vback-porch = <0>`). Without the offset a band of
stale pixels shows in one column — the bench ruler pattern is flush at
both ends with kx=14/ky=0. All other `panel-timing` props must stay 0
(nonzero hsync/vsync lens or front porches → probe fails with
"panel-timing out of bounds"), and `width-mm`/`height-mm` are mandatory
(0/0 legal). `spi-max-frequency` defaults to 16 MHz (panel spec 32 MHz);
lower it on long flying leads if the glass shows pixel noise — the bench
setup ran `dtoverlay=nv3007,speed=2000000`.

**Init-sequence firmware (panel stays dark without it):**
`panel-mipi-dbi` has no built-in NV3007 init code; at probe it requests
`/lib/firmware/panel-mipi-dbi-spi.bin` (the name is hardwired to the DT
compatible string). The file is the vendor init sequence in the
`mipi_dbi_commands` blob format documented in
`drivers/gpu/drm/tiny/panel-mipi-dbi.c` (command/length/payload
records, version 1; delays encoded as NOP commands). It ships in the
image via the `solar-panel-fw` recipe — generated (never hand-edited)
by `fw/tools/lvgl-nv3007-to-mipi-dbi.py` from LVGL 9.4's in-tree NV3007
init list; re-run it with `--dump` when bumping LVGL. Without the file
the driver logs `No config file found for compatible
'panel-mipi-dbi-spi'` (backlight on, panel blank) and retries
`request_firmware()` every minute, so dropping a blob into
`/lib/firmware/` lights the panel without a reboot.

Check after boot: `dmesg | grep -iE "mipi-dbi|panel|fb0"`, then
`fbset -info` (expect 142×428) and a pixel smoke test:
`dd if=/dev/urandom of=/dev/fb0 bs=121552 count=1` (142×428 RGB565).

**Orientation:** the panel is portrait 142×428 and `panel-mipi-dbi`
has no DT `rotation` property (unlike the old ili9341 overlay). For a
landscape UI, rotate in the application's draw path — at 121 kB per
frame this is cheap; a MADCTL 0x36 `MV` bit baked into the init blob
is worth testing too, but the driver re-sends its own MADCTL on
enable, so software rotation is the reliable route.

### RS485 inverter bus (ttyAMA0 / Modbus RTU)

**Now (bring-up):** use your USB-RS485 adapter on the Zero's USB port
(OTG cable) — FTDI/CH34x/CP210x/PL2303/CDC-ACM drivers and `mbpoll`
are in the image:

```sh
mbpoll -a 3 -b 9600 -t 4 -r 1 /dev/ttyUSB0     # read holding reg 1 of inv 3
```

**Native (later):** the `solar-rs485` overlay moves the PL011 onto
the header pair and uses RTS as the transceiver direction signal:

| MAX485-style transceiver | GPIO | physical pin |
| --- | --- | --- |
| DI | GPIO14 (TXD0) | P1-08 |
| RO | GPIO15 (RXD0) | P1-10 |
| DE + !RE | GPIO17 (RTS0) | P1-11 |
| A / B | bus A / bus B (+ 120 Ω termination at both ends) | — |
| VCC | 5 V from P1-02 for 5 V MAX485 boards (RO then needs a divider or level-shifted module!) | — |

Enable by uncommenting the `#dtoverlay=solar-rs485` line in
`config.txt` (on the target: `mount /dev/mmcblk0p1 /mnt &&
sed -i 's/^#dtoverlay=solar-rs485/dtoverlay=solar-rs485/'
/mnt/config.txt && reboot`), or flip it in `RPI_EXTRA_CONFIG` in
`kas-rpi0.yml` and rebuild — **this steals GPIO14/15 from the
mini-UART console** (the overlay disables it; there is no other
pin pair on this board, so you'll be working over SSH after that).
RTS idles asserted (active-low pad ⇒ LOW = driving); for DE-on-RTS
MAX485 wiring that means the driver is enabled when idle — set
`solar-rs485` + `rs485ctl /dev/ttyAMA0 -e -i` (inverted: released
while driving… experiment — see `rs485ctl -h`), and check with a
scope on RO/DE before connecting the inverter.

The kernel RS485 mode (auto-RTS per frame) is configured with
`rs485ctl` (in the image):

```sh
stty -F /dev/ttyAMA0 9600
rs485ctl /dev/ttyAMA0 -e -n --send-delay 1 --after-delay 1
mbpoll -a 3 -b 9600 -t 4 -r 1 /dev/ttyAMA0
```

## Requirements

- `kas` (pip or distro package; tested with 5.5), git, python3
- ~50 GB free disk; internet access (GitHub + source tarballs)
- First full build: ~20–40 min on a fast machine (subsequent: minutes)

## Build

1. Set your WiFi credentials — **keep them out of the committed yml**.
   The kas file declares `SOLAR_WIFI_SSID/PSK/COUNTRY` in its `env:`
   section (placeholders), so plain environment variables override them
   and are passed through to the build — no credential file at all. The
   Zed **Build FW** task does exactly this (its env block lives in the
   gitignored `.zed/tasks.json`):

   ```sh
   SOLAR_WIFI_SSID="my-network" \
   SOLAR_WIFI_PSK="0123...64-hex-from-wpa_passphrase" \
   SOLAR_WIFI_COUNTRY=DK \
   kas build fw/kas-rpi0.yml
   ```

   For non-interactive builds there is also the kas-fragment route:
   create `.kas-wifi-local.yml` (gitignored) next to `kas-rpi0.yml` and
   build with `kas build fw/kas-rpi0.yml:.kas-wifi-local.yml`:

   ```yaml
   header:
     version: 17
   local_conf_header:
     wifi-local: |
       SOLAR_WIFI_SSID = "my-network"
       SOLAR_WIFI_PSK = "0123...64-hex-from-wpa_passphrase"
       SOLAR_WIFI_COUNTRY = "DK"   # ISO alpha-2 regulatory domain
   ```

   Generate the hex PSK with: `wpa_passphrase "SSID" 'pass' | sed -n 's/.*psk=//p'`
   (A plain passphrase works too — the recipe writes hex PSKs raw and quotes
   passphrases, per wpa_supplicant's syntax rules.)

2. Build from the repo root:

   ```sh
   kas build fw/kas-rpi0.yml                          # placeholder wifi
   kas build fw/kas-rpi0.yml:.kas-wifi-local.yml      # your real wifi (fragment route)
   ```

## Flash

The image is a `.wic.bz2` (p1 vfat boot + squashfs-xz root slots p2/p3 +
ext4 `/data` on p4):

```sh
ls build/tmp/deploy/images/raspberrypi0-wifi/solar-ctl-image-raspberrypi0-wifi.rootfs.wic.bz2

bmaptool copy \
  build/tmp/deploy/images/raspberrypi0-wifi/solar-ctl-image-raspberrypi0-wifi.rootfs.wic.bz2 \
  /dev/sdX          # the raw SD device, NOT a partition like /dev/sdX1
```

Fallback without `bmaptool`: `bzcat <image>.wic.bz2 | sudo dd of=/dev/sdX bs=4M status=progress`
(sync afterwards; no need to decompress first).

## First boot / console

- **Serial console (easy mode, dev phase)**: UART is enabled
  (`ENABLE_UART = "1"`) on the GPIO header pins 6/8/10 (GND/TXD/RXD),
  115200 8N1. `root` **auto-logs-in on the serial getty** — plug in a
  cable and you are in, no password. Lab convenience from
  `IMAGE_FEATURES += "empty-root-password serial-autologin-root"`; strip
  both before anything leaves the bench.
- **Network**: on boot, `solar-wifi.service` starts `wpa_supplicant` on
  `wlan0` and `solar-wifi-dhcp.service` requests a lease via udhcpc.
  Check with `ip addr show wlan0` (from serial).
- **SSH — root-only, key-only**: dropbear runs with `-s` (password logins
  disabled; root key login allowed — OE's default `-w` would lock root out
  entirely), and `/root/.ssh/authorized_keys` is baked by the
  `solar-rootkeys` recipe from the `SOLAR_SSH_PUBLIC_KEY` build variable
  (declared in `fw/kas-rpi0.yml` `env:`, set like `SOLAR_WIFI_*` — in the
  `.zed/tasks.json` "Build FW" env block or a local kas fragment; several
  public keys separated by `\n`, or an absolute path to a pub key file
  (same content-or-path convention as `SOLAR_SWU_*`; `$HOME/...` is NOT
  expanded anywhere — Zed task env does not do it and the recipe won't
  compensate). No other account has a key or usable
  password, so root-with-key is the only way in:

  ```sh
  ssh -i ~/.ssh/id_ed25519 root@<board>
  ```

  Public keys only — the private half must never enter a build. Unset =
  no `authorized_keys` file exists at all (the CI default): with `-s`,
  SSH logins become flatly impossible — deliberate policy: this repo
  contains NO keys at all, no key = no access (serial console unaffected).
  Prefer plain ed25519/RSA keys: FIDO `sk-*` keys verify natively on
  dropbear ≥ 2025.x (`DROPBEAR_SK_KEYS`, on by default, no libfido2) but
  demand a touch on **every** connection. `/root` sits on the read-only
  squashfs root — authorized_keys cannot be appended on a running target;
  changing it means a rebuild.

## Changing WiFi later

Three options:

1. **Rebuild**: change the `SOLAR_WIFI_*` values in your build task env
   (or your `.kas-wifi-local.yml` fragment), `kas build` again (fast,
   everything is cached except the image). Note: the PSK lives in
   the image (root-only 0600 file) — treat built images as secret.
2. **On the target** (persists across reboots, not across reflashes):
   ```sh
   wpa_passphrase "new-ssid" "new-pass" >> /etc/wpa_supplicant/wpa_supplicant-wlan0.conf
   systemctl restart solar-wifi.service solar-wifi-dhcp.service
   ```
3. **Ad-hoc** without editing files: `wpa_cli` (`add_network`, `set_network`,
   `select_network`).

## A/B updates & signing

The image ships the A/B layout (`files/wic/solar-ctl-ab.wks`): p1 vfat (GPU
firmware, U-Boot, fallback `uImage`, pre-seeded `uboot.env`) + p2/p3
squashfs-xz root slots + p4 ext4 `/data` holding the `/etc` overlay upper and
the **per-slot kernels** (`/data/cores/slot-{a,b}/uImage`). U-Boot env `slot=a|b`
picks the slot; the kernel is loaded from `/data` via `ext4load`, so normal
updates never write FAT. SWUpdate's U-Boot backend writes `slot` itself (a
`bootenv` entry in the signed sw-description, flushed only after all images
installed OK), and the p4 `/data`
is self-healed at preinit (`e2fsck`, reformat only as last resort). Full
decision record: [`docs/swupdate-ota.md`](docs/swupdate-ota.md).

Applying an update (on the board):

```sh
solar-update -i /path/to/solar-ctl-image.swu           # local file, reboots on success
solar-update -d "-u https://host/solar-ctl-image.swu"  # or pull from a URL
solar-update -n -i /path/to/solar-ctl-image.swu        # stage only, reboot manually
```

`solar-update` derives the target from `root=` in `/proc/cmdline` (running on
p2 → install set `alt`/slot-b and vice versa) and holds `/boot` rw long enough
for the env write. The reboot is done by `solar-swupdate-progress.service`: it
watches swupdate's progress socket, logs every update to journald, and on
SUCCESS runs a gate script that calls `systemctl reboot` (SWUpdate itself never
reboots). `-n|--no-reboot` plants a one-shot `/run/solar-update/no-reboot` the
gate consumes instead of rebooting. The
`.swu` is ONE file for both
slots (libconfig sets, selected with `-e stable,main|alt`), signed RSA-4096 /
SHA-256 and verified against `/etc/solar/swupdate.pub.pem`; SWUpdate runs
on-demand only (its daemon units are stripped from the image; the progress
service is receive-only, nothing accepts update commands).

**Signing keys** (same policy as WiFi creds — never committed):

```sh
fw/tools/swu-keygen.sh                      # RSA-4096 (NOT ed25519: SWUpdate
                                            # and U-Boot FIT have no ed25519;
                                            # same keypair later signs FIT too)
# local:  put the private key (PEM content or absolute path) in
#         .zed/tasks.json env as SOLAR_SWU_PRIVATE_KEY; the public key is
#         derived (set SOLAR_SWU_PUBLIC_KEY only if you want it cross-checked)
# CI:     gh secret set SOLAR_SWU_PRIVATE_KEY < private.pem
```

Without the env var the build **fails** (hard `bb.fatal`) — deliberately: the
repo ships no keypair of any kind, so a signed artifact can never come from a
key that merely happened to be committed. Flash a
milestone card with the usual `bmaptool` procedure (`.wic.bz2` + `.bmap`).

Before touching the layout on older hardware, run the read-only probe and
keep the output:

```sh
scp fw/tools/ota-probe.sh root@<board>:/tmp/ && ssh root@<board> sh /tmp/ota-probe.sh
```

## Design notes / caveats

- **No poky**: oe-core ships no distro conf, so we provide our own
  (`conf/distro/solar-ctl.conf`) with `TCLIBC=musl` and
  `INIT_MANAGER=systemd`. Busybox is OE-core's default provider — we stay
  lean by pulling only `packagegroup-core-boot` via `core-image-minimal`.
- **musl + systemd** builds cleanly here (verified end-to-end), but
  bitbake warns it is *experimental* upstream. If a future recipe needs
  glibc-only bits, expect to patch or drop it.
- **Kernel modules**: meta-raspberrypi pulls *all* ~1800 modules into every
  rpi image; our image recipe removes that and installs only the `brcmfmac`
  stack plus `linux-firmware-rpidistro-bcm43430` (do not rely on the machine
  conf RRECOMMENDS — the 2026-09-18 image omitted the firmware). Add specific
  `kernel-module-*` packages to `solar-ctl-image.bb` as needed.
- **bitbake** has no `wrynose` branch; we pin `2.18`, enforced by oe-core's
  sanity checker.
- **Licenses**: `LICENSE_FLAGS_ACCEPTED = "synaptics-killswitch"` is set for
  `linux-firmware-rpidistro`; review the BSP's `docs/ipcompliance.md`
  before shipping.
- The `solar-ctl` repo entry has no `url`, so kas builds straight from your
  working tree. Run `kas lock` for reproducible builds of the upstream pins.
