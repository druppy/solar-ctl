# solar-ctl firmware (fw/)

Yocto **kas** build for the **Raspberry Pi Zero W** inverter controller,
on the **wrynose** release. Standalone **openembedded-core** — *no poky* —
with a deliberately minimal image (~120 packages, ~48 MB compressed):
**musl** instead of glibc, **busybox**, **systemd**.

## CI / releases

`.github/workflows/firmware.yml` builds the image with
`kas build fw/kas-rpi0.yml` (plus a throwaway WiFi fragment when the repo
secrets are set):

- **push/PR to main** — build + `solar-ctl-image-<sha>` artifact (48 MB zip
  contents: `wic.bz2`, `bmap`, `manifest`, `SHA256SUMS`)
- **tag `v*`** — same build, additionally published as a GitHub Release
- **caching**: only `build/sstate-cache` (~1.3 GB) is cached —
  `build/downloads` is ~9.5 GB (8.4 GB of it is `git2` bare clones) and
  GitHub's per-repo cache quota is 10 GB, so caching downloads sat at
  9.97 GiB with zero headroom. `restore-keys` fall back to the newest
  `yocto-*` cache when the key rotates, and saves carry a `-<sha>` suffix
  so every push refreshes the cache (sstate is content-addressed — a
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

| Path | Purpose |
| --- | --- |
| `kas-rpi0.yml` | **Build this** — repos (wrynose branch tips), machine, WiFi creds, local.conf bits |
| `conf/layer.conf` | `fw/` is a small meta layer (`solar-ctl`) |
| `conf/distro/solar-ctl.conf` | Our distro: `TCLIBC=musl`, `INIT_MANAGER=systemd`, lean distro features |
| `recipes-core/images/solar-ctl-image.bb` | Minimal image (dropbear ssh, WiFi) |
| `recipes-connectivity/solar-wifi/` | WiFi bring-up: supplicant config + systemd units |
| `recipes-core/dropbear/` | bbappend: root-only key-only SSH config |
| `recipes-core/solar-rootkeys/` | `/root/.ssh/authorized_keys` (FIDO sk-key, public half) |
| `recipes-kernel/linux/` | Kernel config slimming + nv3007/solar-rs485 DT overlays (142×428 panel-mipi-dbi TFT) |
| `recipes-support/rs485ctl/` | RS485 RTS direction-control setup tool |
| `docs/swupdate-ota.md` | **A/B OTA design record** (squashfs roots, `/etc` overlay, SWUpdate, tryboot) |
| `tools/ota-probe.sh` | Read-only on-target probe of boot chain/filesystems (run before OTA work) |

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

### NV3007 2.79" TFT (142×428 SPI, enabled by default)

TZT 2.79" 142×428 SPI display with the NV3007 controller. It is driven
by the generic in-kernel `panel-mipi-dbi` driver (not `ili9341` — that
one hardcodes 240×320) via our `nv3007` overlay, which is **on** in
`config.txt`. The resolution comes from the overlay's `panel-timing`
node; the controller init sequence comes from a firmware file, see
below:

| display pin | GPIO | physical pin | overlay override |
| --- | --- | --- | --- |
| MOSI (SDA) | GPIO10 | P1-19 | — |
| SCLK (SCK) | GPIO11 | P1-23 | — |
| CS  | GPIO8 | P1-24 | — |
| DC  | GPIO25 | P1-22 | `dc_pin=<n>` |
| RST | GPIO24 | P1-18 | `reset_pin=<n>` |
| BLK | GPIO18 | P1-12 | `led_pin=<n>` (0 = tie to 3V3) |
| VCC | — | 3V3 (P1-01/17) | — |
| GND | — | P1-06/09/14/20/25/30/34/39 | — |

Runs at 3.3 V — do not feed 5 V into data lines unless your module
board is explicitly 5 V-tolerant. Backlight is driven from GPIO18 via
`gpio-backlight` (on at boot); wiring BLK straight to 3V3 also works —
the GPIO then just toggles a disconnected pin.

**Init-sequence firmware (panel stays dark without it):**
`panel-mipi-dbi` has no built-in NV3007 init code; at probe it requests
`/lib/firmware/panel-mipi-dbi-spi.bin` (the name is hardwired to the DT
compatible string). The file is the vendor init sequence in the
`mipi_dbi_commands` blob format documented in
`drivers/gpu/drm/tiny/panel-mipi-dbi.c` (command/length/payload
records, version 1; delays encoded as NOP commands). It is **not**
yet baked into the image — until it is, the driver logs
`No config file found for compatible 'panel-mipi-dbi-spi'` and the
backlight comes on but the panel stays blank. To test with a blob
handy: `scp blob root@<host>:/lib/firmware/panel-mipi-dbi-spi.bin`
— the driver retries `request_firmware()` every minute, no reboot
needed.

Check after boot: `dmesg | grep -iE "mipi-dbi|panel|fb0"`, then
`fbset -info` (expect 142×428) and a pixel smoke test:
`dd if=/dev/urandom of=/dev/fb0 bs=1024 count=119`.

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
cd /etc && rs485ctl /dev/ttyAMA0 -e -n --send-delay 1 --after-delay 1
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

2. Build from the repo root:

   ```sh
   kas build fw/kas-rpi0.yml                          # placeholder wifi
   kas build fw/kas-rpi0.yml:.kas-wifi-local.yml      # your real wifi (fragment route)
   ```

## Flash

The image is a hybrid `.wic.bz2` (FAT32 boot partition + ext4 rootfs):

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
  entirely), and `/root/.ssh/authorized_keys` from the
  `solar-rootkeys` recipe holds the maintainer's FIDO security key
  (`sk-ssh-ed25519`). No other account has a key or usable password, so
  root-with-key is the only way in:

  ```sh
  ssh -i ~/.ssh/id_ed25519_sk root@<board>   # touch the security key when prompted
  ```

  Adding/removing people = adding/removing public key lines in
  `recipes-core/solar-rootkeys/solar-rootkeys/root_authorized_keys`
  (public keys only — safe to commit). Rebuild and reflash, or append
  directly to `/root/.ssh/authorized_keys` on the target for a quick
  change. Note: dropbear ≥ 2025.x verifies sk-* keys natively
  (`DROPBEAR_SK_KEYS`, on by default) — no libfido2 on the target.

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

## A/B updates (in design)

The disk layout today is the bare meta-raspberrypi default: one vfat boot
partition + one ext4 rootfs (`sdimage-raspberrypi.wks`), which means "update"
currently means "reflash". The plan to replace it — two read-only
**squashfs** root slots, `/etc` as an **overlayfs** on a writable partition,
a journalled ext4 `/data`, **SWUpdate** as the update agent, and slot
switching done by the **Raspberry Pi firmware** (`tryboot`, since there is no
U-Boot) — is recorded in [`docs/swupdate-ota.md`](docs/swupdate-ota.md),
including which facts are verified and which are still bench questions.

Nothing is implemented yet. Before touching the layout, run the read-only
probe on current hardware and keep the output:

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
  WiFi stack (BCM43430 firmware comes from the machine conf). Add specific
  `kernel-module-*` packages to `solar-ctl-image.bb` as needed.
- **bitbake** has no `wrynose` branch; we pin `2.18`, enforced by oe-core's
  sanity checker.
- **Licenses**: `LICENSE_FLAGS_ACCEPTED = "synaptics-killswitch"` is set for
  `linux-firmware-rpidistro`; review the BSP's `docs/ipcompliance.md`
  before shipping.
- The `solar-ctl` repo entry has no `url`, so kas builds straight from your
  working tree. Run `kas lock` for reproducible builds of the upstream pins.
