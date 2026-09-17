# solar-ctl dropbear tweaks:
# Our admin key is a FIDO security key (sk-ssh-ed25519@openssh.com).
# Dropbear 2025.x verifies sk-* keys natively and server-side
# (DROPBEAR_SK_KEYS, on by default in src/default_options.h) - no extra
# dependencies or configure options needed; verified against 2025.89.

# Replace OE's /etc/default/dropbear with ours (same file name, and
# FILESEXTRAPATHS below makes this layer's copy win at unpack time).
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI += "file://dropbear.default"
