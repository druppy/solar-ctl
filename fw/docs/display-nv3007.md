# NV3007 display bring-up — the story so far

Timeline: driver chosen 2026-09, glass first lit 2026-10-01/02, UI verified
on the bench **2026-10-02**, speckle-line investigation closed as hardware
**2026-10-05** (see its section). This file records *what the problems
actually were*, in the order they cost us time, so the next person (or the
next me) does not re-pay them. The terse facts also live in `.rules` and
`fw/README.md`; this is the narrative version.

---

## The working stack, end to end

```
inv_ctl (LVGL 9.4, fbdev backend)
  -> /dev/fb0                          (DRM fbdev emulation)
    -> panel-mipi-dbi (kernel DRM driver, generic MIPI-DBI SPI)
      -> nv3007-overlay.dts            (fw/recipes-kernel/linux/files/)
      -> /lib/firmware/panel-mipi-dbi-spi.bin   (the init sequence!)
```

| Piece | Where | Note |
| ------- | ---------------------------------------------------------------- | ------------------------------ |
| Panel | TZT 2.79" **142×428 portrait**, NV3007 controller, 8-pin SPI | **no MISO** — write-only bus |
| DT overlay | `fw/recipes-kernel/linux/files/nv3007-overlay.dts` | built as `nv3007.dtbo` |
| Init blob | `solar-panel-fw` → `/lib/firmware/panel-mipi-dbi-spi.bin` | generated, never hand-edited |
| Blob generator | `fw/tools/lvgl-nv3007-to-mipi-dbi.py` | parses LVGL 9.4's `lv_nv3007.c` |
| Kernel slimming | `fw/recipes-kernel/linux/files/solar-ctl-slim.cfg` | keeps `DRM_PANEL_MIPI_DBI=m` |
| Module packages | `kernel-module-{panel-mipi-dbi,spi-bcm2835,gpio-backlight}` | in the image recipe |
| config.txt | `dtoverlay=nv3007` via `RPI_EXTRA_CONFIG` in `fw/kas-rpi0.yml` | speed override optional |

Bench verdict 2026-10-02, fresh flashed image: glass lit at the image default
**16 MHz SPI** with no pixel noise, ruler pattern flush at both ends, watch UI
upright and aligned.

---

## The traps, in order of encounter

Each one is symptom → cause → fix, because the symptom is what you will
recognise when it comes back.

### 1. Absolutely nothing lit — it was wiring

SDA(MOSI) had been plugged into **P1-17, which is 3V3**, not P1-19. The
panel cannot fail more quietly than this. Boring, first, and expensive in
wall-clock. Verify pin numbers against the header table in `fw/README.md`
before believing any software theory.

### 2. Probe returns -EINVAL with no helpful message

The `panel-mipi-dbi` binding (kernel 6.18) is strict in ways the docs do not
shout about:

- `width-mm` / `height-mm` on the panel node are **mandatory properties**.
  Missing → probe `-EINVAL` and that is the entire error report. `0/0`
  ("unknown size") is legal — that is what the overlay does.
- Every `panel-timing` property must be **0 except** `hactive`/`vactive` and
  the two back-porches. A textbook timing node (hsync/vsync lens, front
  porches) → `panel-timing out of bounds` → probe `-EINVAL`. The kernel
  rejects, it does not negotiate.

### 3. A band of stale pixels along one edge — GRAM offset

The TZT module's visible window does **not** start at GRAM (0,0): it sits at
x=14. The controller happily accepts pixels you will never see and shows
uninitialized GRAM where you do. Fix in the overlay: `hback-porch = <14>`
(`vback-porch = <0>` — ky=0 for this module). Since all other timing props
must be 0 anyway, the back-porches are *only* doing this job — they are a
GRAM offset here, not timing.

How we saw it: paint a pattern with row 0 and the last row coloured and the
left/right edges strobed ("ruler"), `dd` it to `/dev/fb0`, look at which end
is short. Pattern is RGB565 little-endian, exactly **121552 bytes**
(142×428×2). After the fix: flush at both ends.

### 4. Init silently skipped — the bus cannot read back

`panel-mipi-dbi` calls `mipi_dbi_display_is_on()` before sending the init
list. On a write-only bus (our 8-pin header has no MISO) that read floats and
can come back "already on" — init never runs, no error anywhere. The binding
has exactly the right knob: **`write-only;`** in the overlay. Mandatory here.

### 5. The init sequence itself — the firmware-file mechanism

`panel-mipi-dbi` carries **no panel init code**. The sequence lives in a
firmware file named after the compatible string:
`/lib/firmware/panel-mipi-dbi-spi.bin` (= `compatible[0]` + `.bin`), loaded
with `request_firmware()` — and if missing, the driver retries about every
60 s, so a dropped-in blob lights the glass without a reboot.

Consequences of that design:

- A new image with no blob = backlight on, panel dark, one friendly line in
  `dmesg` (`No config file found for compatible 'panel-mipi-dbi-spi'`).
- **Never invent NV3007 register values.** The blob is *generated* by
  `fw/tools/lvgl-nv3007-to-mipi-dbi.py` from LVGL's in-tree driver init list
  (itself descended from Arduino_GFX, i.e. field-proven) straight into the
  kernel's binary firmware format (`"MIPI DBI"` magic + version + TLV-ish
  cmd/param records; delays are NOP commands in **ms**). Two converter
  subtleties burned in advance: LVGL delays are in **10 ms units** (so
  post-SLPOUT `22` means 220 ms), and bumping LVGL means re-running the tool
  with `--dump` and reviewing the output.
- Kernel firmware format authority: `drivers/gpu/drm/tiny/panel-mipi-dbi.c`.

### 6. The reset-polarity rabbit hole (a non-problem we spent time on)

LVGL's nv3007 driver contains **no reset code at all**, and the kernel's
`mipi_dbi_hw_reset()` idles the line physically HIGH with a ~20 µs LOW pulse
— which is exactly what the module's active-low RES wants. Therefore
`reset-gpios = <&gpio 24 GPIO_ACTIVE_HIGH>` was **correct all along**;
flipping it to `ACTIVE_LOW` was always wrong. Documented in the overlay so
this theory never gets re-litigated.

### 7. kbuild: "No rule to make target `nv3007.dtbo`"

An overlay only builds if registered in
`arch/arm/boot/dts/overlays/Makefile`'s `dtbo-y` list — we do it with
`0001-overlays-register-nv3007-and-solar-rs485.patch`. Two sharp edges:

- naming: kbuild maps `x.dtbo` ← `x-overlay.dts`, so our
  `nv3007-overlay.dts` registers as **`nv3007.dtbo`**, not
  `nv3007-overlay.dtbo`.
- kernel-yocto **commits** SRC_URI patches onto a shared `kernel-source`
  branch; if the patch changes, stale commits linger — `git reset --hard
  <SRCREV>` in `build/tmp/work-shared/.../kernel-source` before rebuilding.

And the overlay itself must be added to `RPI_KERNEL_DEVICETREE_OVERLAYS`
in **kas `local_conf_header`**, not the kernel bbappend — boot files are
assembled in the image recipe, which never sees kernel-recipe appends.

### 8. Terminal text painted onto the panel beside the UI

The kernel framebuffer console (fbcon) binds the VT to whatever registers as
`/dev/fb0` and cheerfully draws boot messages and a login prompt **onto the
display**, under/over the LVGL scene. Runtime workaround (what we did on the
bench): `echo 0 > /sys/class/vtconsole/vtcon1/bind`. Real fix (in tree):
`# CONFIG_FRAMEBUFFER_CONSOLE is not set` in `solar-ctl-slim.cfg` —
`DRM_FBDEV_EMULATION` does **not** select it, so `/dev/fb0` survives for
LVGL and console life stays on the serial UART. Only after that kernel is
flashed is the panel text-free for good.

### 9. SPI speed

Panel spec allows 32 MHz; during bring-up the bench ran
`dtoverlay=nv3007,speed=2000000` on flying leads (pixel noise on glass = the
tell for "too fast"). Production default is now **16 MHz** (a judgment call,
bench-confirmed clean on the 2026-10-02 flash). Override with
`dtoverlay=nv3007,speed=<Hz>` if a future build shows noise on new wiring.

### 10. `/dev/fb0` appears late — inv_ctl must wait

fb0 only exists after panel probe **plus** the firmware-file load, which can
lag boot by the retry interval. The `inv-ctl` fb backend therefore retries
opening `/dev/fb0` forever instead of assuming it at startup. Do not
"optimise" that away.

### 11. `--` in the signal field (UI-adjacent, not display)

Found while verifying the UI: `/proc/net/wireless` prints the interface as
`%-8s: ` — space-**padded before the colon** — and the link-quality column
can print as `44/70`. A `"%63[^:]:"`+`strcmp` parse silently never matches
and the quality column breaks naive numeric scans. `net.cpp` parses
token-wise now.

---

## The speckled white line on one long edge — closed: hardware

Symptom: one row of speckled white pixels along one long edge of the portrait
panel, on every boot, on **three separate physical modules**, independent of
framebuffer content. It looks exactly like the GRAM-offset band of trap 3 —
which is why it cost us two days. It is not.

Evidence that exonerates software, each independently bench-checked:

| # | Test | Result |
| --- | ------------------------------------------------------------------ | -------------------------------------------- |
| 1 | DT window widened (`hback-porch` 13/win 144, then 11/win 148)      | line unmoved — not an offset problem         |
| 2 | Ruler pattern: fb edge columns uniformly dark, in and out of window | content clean — line is not coming from fb   |
| 3 | Stripes painted into fb cols *outside* the DT window               | invisible on glass — GRAM addressing correct |
| 4 | SPI clock 2 MHz vs 16 MHz                                          | no effect — not signal integrity             |
| 5 | VDD from a clean external 3V3 instead of the Pi rail               | no effect — not supply noise                 |
| 6 | MADCTL MX mirror flipped (live blob patch + driver rebind)         | line glued to same *physical* edge           |
| 7 | SOUCTRL column windows E0h–F2h (datasheet §6.6.1) → reset defaults | no effect — not our column tuning            |
| 8 | Three separate modules                                             | identical line — not unit damage             |
| 9 | Vendor-published NV3007A SPI init for this exact glass             | byte-for-byte **identical** to our blob      |

Test 9 is the clincher: `fw/docs/nv3007a-ivo-spi-init.txt` (OSPtek module
YDP279B001-V2, repo `osptek/tft-2.79-142x428-spi-nv3007`, CC BY 4.0) is the
module vendor's own init for this glass and diffs identical to our shipped
120-command blob (only delta: a `Delay(200)` after `DISPON`). The init
sequence is exonerated *by identity*, not by inference — there is no init
sequence that could fix this, because we already ship theirs.

**Verdict:** source-driver / COF bonding artifact of the TZT product line.
Mitigate mechanically (bezel hiding that edge) or switch module family. Do
**not** re-litigate in software; all levers above are exhausted, and `.rules`
forbids fabricating NV3007 register values anyway.

Tooling built for the hunt, worth keeping:

- `fw/tools/fb-dump.sh` — captures `/dev/fb0` as base64 (raw bytes through
  an interactive pty are silently mangled; this is the only reliable way to
  get "eyes on glass" over SSH).
- Live blob experiments: patched bin → `/data/fw/`, `mount --bind /data/fw
  /lib/firmware`, unbind/bind `/sys/bus/spi/drivers/panel-mipi-dbi`, restart
  `inv-ctl`. Reverted by reboot, ideal for register what-ifs (tests 6 and 7
  ran this way).
- The controller datasheet for register archaeology: `fw/docs/NV3007.pdf`.

---

## Orientation

`panel-mipi-dbi` has **no DT rotation property**. Rotate in software
(LVGL) — or accept native portrait, which is what the UI does today.

## What OTA does *not* carry

The `.dtbo`, the base DTB and all GPU boot files live on p1 and are **never
part of an update** (see `ab-boot-uboot.md`). A kernel delivered by OTA must
stay compatible with the dtb on the installed card; changing the panel
overlay means reflashing p1 on the bench.

## Watch list

- LVGL bump → re-run the converter with `--dump`, review the init list.
- A different TZT panel batch could have a different GRAM offset — the ruler
  pattern re-verifies kx/ky in five minutes.
- 16 MHz is validated on *this* bench wiring; new physical layout = one
  ruler pattern + one pixel-noise look.
- The speckle edge: accepted as hardware. If the product housing ever gets
  a bezel, orient it over the affected long edge (left in portrait, as
  delivered).
