# solar-ctl firmware (fw/)

Yocto **kas** build for the **Raspberry Pi Zero W** inverter controller,
on the **wrynose** release. Standalone **openembedded-core** — *no poky* —
with a deliberately minimal image (~100 packages, ~48 MB compressed):
**musl** libc, **busybox**, **systemd**.

## CI / releases

`.github/workflows/firmware.yml` builds the image with
`kas build fw/kas-rpi0.yml:fw/kas-ci.yml` (the `kas-ci.yml` fragment only
keeps the sstate/download cache for Actions to reuse):

- **push/PR to main** — build + `solar-ctl-image-<sha>` artifact (48 MB zip
  contents: `wic.bz2`, `bmap`, `manifest`, `SHA256SUMS`)
- **tag `v*`** — same build, additionally published as a GitHub Release
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
| `kas-rpi0.yml` | **Build this** — repos (pinned to wrynose), machine, WiFi creds, local.conf bits |
| `kas-baseline.yml` | Reference copy of the repo pinning (kas ≥ 5 can't auto-include fragments) |
| `conf/layer.conf` | `fw/` is a small meta layer (`solar-ctl`) |
| `conf/distro/solar-ctl.conf` | Our distro: `TCLIBC=musl`, `INIT_MANAGER=systemd`, lean distro features |
| `recipes-core/images/solar-ctl-image.bb` | Minimal image (dropbear ssh, WiFi) |
| `recipes-connectivity/solar-wifi/` | WiFi bring-up: supplicant config + systemd units |
| `recipes-core/dropbear/` | bbappend: root-only key-only SSH config |
| `recipes-core/solar-rootkeys/` | `/root/.ssh/authorized_keys` (FIDO sk-key, public half) |

## Requirements

- `kas` (pip or distro package; tested with 5.5), git, python3
- ~50 GB free disk; internet access (GitHub + source tarballs)
- First full build: ~20–40 min on a fast machine (subsequent: minutes)

## Build

1. Set your WiFi credentials — **keep them out of the committed yml**.
   Create `.kas-wifi-local.yml` (gitignored) next to `kas-rpi0.yml`:

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
   kas build fw/kas-rpi0.yml:.kas-wifi-local.yml      # your real wifi
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
- **SSH — root-only, key-only**: dropbear runs with `-B` (all password
  auth refused), and `/root/.ssh/authorized_keys` from the
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

1. **Rebuild**: edit `SOLAR_WIFI_*` in `kas-rpi0.yml`, `kas build` again
   (fast, everything is cached except the image). Note: the PSK lives in
   the image (root-only 0600 file) — treat built images as secret.
2. **On the target** (persists across reboots, not across reflashes):
   ```sh
   wpa_passphrase "new-ssid" "new-pass" >> /etc/wpa_supplicant/wpa_supplicant-wlan0.conf
   systemctl restart solar-wifi.service solar-wifi-dhcp.service
   ```
3. **Ad-hoc** without editing files: `wpa_cli` (`add_network`, `set_network`,
   `select_network`).

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
