# fbdump - on-device framebuffer screenshot as PNG (repo-root fbdump/,
# delivered via file:// - the solar-ctl repo deliberately has no git URL in
# kas, see .rules; same SRC_URI plumbing as the inv-ctl recipe).
#
# Zero DEPENDS by design: the PNG container and the zlib stream (stored
# DEFLATE blocks) are hand-rolled in src/main.cpp, so this adds no
# libpng/zlib to the image.

SUMMARY = "solar-ctl framebuffer screenshot tool (PNG, zero deps)"
DESCRIPTION = "Captures /dev/fb0 (display ground truth for the NV3007 panel) and writes a PNG to a file or to stdout, e.g. 'ssh board fbdump > shot.png'. Device defaults are baked in at compile time (CMake FBDUMP_* cache vars); CLI flags override at runtime."
HOMEPAGE = "https://github.com/druppy/solar-ctl"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=1d87715cd026dc21bccddaf6ecb9b94d"

FILESEXTRAPATHS:prepend := "${@os.path.abspath('${THISDIR}/../../..')}:"
SRC_URI = "file://fbdump"
S = "${UNPACKDIR}/fbdump"

inherit cmake

# Device defaults baked into the binary (the fallback geometry and the
# default -d argument); single-sourced here, mirroring the panel setup
# (142x428 RGB565 on /dev/fb0). The runtime FBIOGET_VSCREENINFO query still
# reports the real geometry; CLI flags win over both.
EXTRA_OECMAKE = "-DFBDUMP_FB_DEV=/dev/fb0 -DFBDUMP_WIDTH=142 -DFBDUMP_HEIGHT=428 -DFBDUMP_BPP=16"
