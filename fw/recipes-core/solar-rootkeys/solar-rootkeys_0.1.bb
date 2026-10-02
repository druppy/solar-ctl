# Sole SSH entry point for solar-ctl: root's authorized_keys.
#
# The key(s) come from SOLAR_SSH_PUBLIC_KEY (declared in fw/kas-rpi0.yml
# `env:`, same env-style pattern as SOLAR_WIFI_* and SOLAR_SWU_*). Value
# resolution mirrors solar-swu-signing.bbclass: EITHER the key CONTENT
# (one "type base64 comment" line per key, several keys separated by
# newlines - JSON-escaped "\n" from .zed/tasks.json is decoded by
# .zed/env-inject.py) OR an ABSOLUTE path to a pub key file on the build
# host. Absolute means absolute: Zed task env does not expand $HOME and
# this recipe deliberately does not compensate (write /home/... or fix
# Zed). PUBLIC keys only - safe to commit/paste anywhere; the private
# half must never enter a build. Like the swu keys, bitbake hashes the
# variable string, not the file behind a path: editing the .pub without
# changing the var requires touching this recipe.
#
# Unset/empty = NO authorized_keys in the image (warning, build proceeds
# - CI has no key by design): with dropbear '-s' (key-only) nobody can
# log in over SSH anymore. That is policy, not an oversight - the repo
# is PUBLIC and holds NO keys at all (no fallback file); no key = no
# access. The serial console is unaffected.
#
# sk-*/FIDO keys verify natively on target (dropbear DROPBEAR_SK_KEYS) but
# cause a signing prompt per connection - prefer plain ed25519/RSA keys.
# NOTE: /root lives on the read-only squashfs root: authorized_keys can
# only be changed by rebuilding, never by appending on the target.
SUMMARY = "Root SSH authorized_keys (from SOLAR_SSH_PUBLIC_KEY)"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/COPYING.MIT;md5=3da9cfbcb788c80a0384361b4de20420"

# Pure file installation: no compiler/libc needed.
INHIBIT_DEFAULT_DEPS = "1"

S = "${UNPACKDIR}"

SOLAR_SSH_PUBLIC_KEY ??= ""
# The key is read by NAME inside python (invisible to vartable hashing),
# so pin it explicitly - same stale-task trap as do_swu_signing_keys.
do_install[vardeps] += "SOLAR_SSH_PUBLIC_KEY"

def solar_ssh_get_keys(d):
    """Resolve SOLAR_SSH_PUBLIC_KEY to a list of key lines:
    content | absolute path | fatal. Returns None when unset -
    the image then gets NO authorized_keys (no key = no access)."""
    import os
    val = (d.getVar("SOLAR_SSH_PUBLIC_KEY") or "").strip()
    if not val:
        return None
    if val.startswith("/"):
        if not os.path.isfile(val):
            bb.fatal("SOLAR_SSH_PUBLIC_KEY names a file that does not exist: %s" % val)
        bb.note("solar-rootkeys: baking pubkey(s) from %s" % val)
        with open(val) as f:
            val = f.read().strip()
    elif not val.startswith(("ssh-", "ecdsa-", "sk-")):
        bb.fatal("SOLAR_SSH_PUBLIC_KEY is neither a public key line (starts "
                 "with 'ssh-'/'ecdsa-'/'sk-') nor an absolute host path "
                 "('$HOME/...' does not work - Zed task env does not expand "
                 "it; write an absolute path). Fix the environment.")
    # tolerate literal "\n" sequences (CI secret pasted without real
    # newlines); splitlines() then covers both forms.
    return [l.strip() for l in val.replace("\\n", "\n").splitlines() if l.strip()]

python do_install() {
    import os
    keys = solar_ssh_get_keys(d)
    sshdir = os.path.join(d.getVar("D"), d.getVar("ROOT_HOME").strip("/"), ".ssh")
    # The dir is always shipped: the image RPM-installs ${PN}, and a
    # zero-file package breaks do_rootfs ("nothing provides ...").
    os.makedirs(sshdir, mode=0o700, exist_ok=True)
    os.chmod(sshdir, 0o700)
    if keys is None:
        bb.warn("SOLAR_SSH_PUBLIC_KEY not set - image gets NO root SSH keys; "
                "dropbear -s then makes SSH logins impossible (policy: no "
                "keys in the repo, no key = no access; serial unaffected).")
        return
    for k in keys:
        if not k.startswith(("ssh-", "ecdsa-", "sk-")):
            bb.fatal("bad SOLAR_SSH_PUBLIC_KEY line %r: want "
                     "'<type> <base64> [comment]', one key per line, "
                     "no wrapping" % k[:48])
    target = os.path.join(sshdir, "authorized_keys")
    with open(target, "w") as fh:
        fh.write("\n".join(keys) + "\n")
    os.chmod(target, 0o600)
}

FILES:${PN} = "${ROOT_HOME}/.ssh"
