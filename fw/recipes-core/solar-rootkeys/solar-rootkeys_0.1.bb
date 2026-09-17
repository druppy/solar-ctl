# Sole SSH entry point for solar-ctl: root's authorized_keys.
# Contains only PUBLIC keys - safe to commit. The matching private keys
# live on FIDO security keys (sk-ssh-ed25519), so authentication also
# requires physical touch of the token.
SUMMARY = "Root SSH authorized_keys (maintainer FIDO security key)"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/COPYING.MIT;md5=3da9cfbcb788c80a0384361b4de20420"

# Pure file installation: no compiler/libc needed.
INHIBIT_DEFAULT_DEPS = "1"

SRC_URI = "file://root_authorized_keys"

do_install() {
    install -d -m 0700 ${D}${ROOT_HOME}/.ssh
    install -m 0600 ${UNPACKDIR}/root_authorized_keys ${D}${ROOT_HOME}/.ssh/authorized_keys
}

FILES:${PN} = "${ROOT_HOME}/.ssh"
