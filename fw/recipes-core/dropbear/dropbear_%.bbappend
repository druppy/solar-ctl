# solar-ctl dropbear tweaks:
# Admin keys are plain public keys baked from SOLAR_SSH_PUBLIC_KEY
# (solar-rootkeys recipe). FIDO sk-* keys would also verify natively
# (DROPBEAR_SK_KEYS is on by default in dropbear 2025.x, verified against
# 2025.89 - no libfido2 needed) but prompt for a touch on every connection.

# Replace OE's /etc/default/dropbear with ours (same file name, and
# FILESEXTRAPATHS below makes this layer's copy win at unpack time).
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI += "file://dropbear.default"
