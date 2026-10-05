#!/bin/sh
# fb-dump.sh - screenshot the NV3007 panel via its framebuffer.
#
# /dev/fb0 IS what the glass shows: panel-mipi-dbi streams it verbatim
# (LVGL does all rotation in software), so this PNG is ground truth for
# display debugging - no camera, no eyeballs required.
#
# Usage: fb-dump.sh [user@host] [out.png]     (defaults: root@192.168.0.53, fb-dump.png)
# Requires: ssh key auth to the target, python3 on the host.
set -e

HOST=${1:-root@192.168.0.53}
OUT=${2:-fb-dump.png}
SSH="ssh -o KexAlgorithms=curve25519-sha256 -o ConnectTimeout=8"

# Geometry from sysfs (virtual_size prints "W,H"); busybox-safe remote side.
GEOM=$($SSH "$HOST" 'echo "$(cut -d, -f1 /sys/class/graphics/fb0/virtual_size) $(cut -d, -f2 /sys/class/graphics/fb0/virtual_size) $(cat /sys/class/graphics/fb0/bits_per_pixel)"')
set -- $GEOM
W=$1 H=$2 BPP=$3
[ -n "$W" ] && [ -n "$H" ] || { echo "fb-dump: cannot read fb0 geometry from $HOST" >&2; exit 1; }

# Binary must NOT cross the pty unencoded: terminal input discipline
# mangles control bytes (empirically 4 non-zero bytes survived 123 KB of
# RGB565 - the dump silently degraded to zeros). base64 is pty-safe.
$SSH "$HOST" 'dd if=/dev/fb0 bs=131072 2>/dev/null | busybox base64' | base64 -d > /tmp/fb-dump.raw

python3 - "$W" "$H" "$BPP" "$OUT" <<'PY'
import struct, sys, zlib
w, h, bpp, out = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
raw = open('/tmp/fb-dump.raw', 'rb').read(w * h * bpp // 8)
if len(raw) < w * h * bpp // 8:
    sys.exit(f'fb-dump: short read ({len(raw)} < {w*h*bpp//8})')
fmt = '<H' if bpp == 16 else '<I'
rows = bytearray()
for y in range(h):
    rows += b'\x00'  # PNG per-scanline filter type: None
    for x in range(w):
        v = struct.unpack_from(fmt, raw, (y * w + x) * bpp // 8)[0]
        if bpp == 16:  # RGB565
            r, g, b = ((v >> 11) & 0x1F) * 255 // 31, ((v >> 5) & 0x3F) * 255 // 63, (v & 0x1F) * 255 // 31
        else:          # XRGB8888
            r, g, b = (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF
        rows += bytes((r, g, b))
def chunk(t, d):
    c = t + d
    return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c))
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(bytes(rows), 9)) + chunk(b'IEND', b'')
open(out, 'wb').write(png)
print(f'{out}: {w}x{h} @{bpp}bpp -> {len(png)} bytes')
PY
