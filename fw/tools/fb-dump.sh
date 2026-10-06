#!/bin/sh
# fb-dump.sh - screenshot the NV3007 panel via its framebuffer.
#
# /dev/fb0 IS what the glass shows: panel-mipi-dbi streams it verbatim
# (LVGL does all rotation in software), so this PNG is ground truth for
# display debugging - no camera, no eyeballs required.
#
# The PNG is rendered ON THE TARGET by fbdump (repo fbdump/, recipe
# solar-fbdump): command-form ssh allocates no pty, so the bytes cross
# binary-clean, and fbdump keeps stdout pure (its info lines land here on
# stderr). Images predating the tool must be OTA'd/flashed to use this.
#
# Usage: fb-dump.sh [user@host] [out.png]     (defaults: root@192.168.0.53, fb-dump.png)
# Requires: ssh key auth to the target.
set -e

HOST=${1:-root@192.168.0.53}
OUT=${2:-fb-dump.png}
SSH="ssh -o KexAlgorithms=curve25519-sha256 -o ConnectTimeout=8"

if ! $SSH "$HOST" 'command -v fbdump >/dev/null 2>&1'; then
    echo "fb-dump: no fbdump on $HOST - OTA/flash an image containing the solar-fbdump package" >&2
    exit 1
fi

$SSH "$HOST" 'fbdump' > "$OUT"
echo "fb-dump: $OUT <- $HOST:fbdump"
