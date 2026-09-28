# SWUpdate signing plumbing for solar-ctl.
#
# Key handling mirrors the WiFi-credential policy (see fw/kas-rpi0.yml `env:`):
# the key material comes from the environment, never from a committed file -
#   SOLAR_SWU_PRIVATE_KEY / SOLAR_SWU_PUBLIC_KEY
# and each value is EITHER the PEM content itself (starts with "-----BEGIN")
# OR an absolute path to a PEM file on the build host. Empty/missing falls
# back to the committed THROWAWAY dev keypair in fw/files/keys/dev with a
# loud warning, so CI and fresh clones always build (fw/tools/swu-keygen.sh
# makes a real one; local = .zed/tasks.json env, CI = Actions secrets).
#
# Why RSA-4096 and not ed25519 (verified against swupdate 2026.05.1 sources):
# the RSA verifier streams EVP_DigestVerifyUpdate/Final, but OpenSSL's ed25519
# API is one-shot only - SWUpdate has no ed25519 path at all (only
# RSA/RSA-PSS/CMS/PKCS7/GPG), `openssl dgst -sign` refuses ed25519 keys, and
# U-Boot FIT sign/verify has no ed25519 either. The SAME keypair is meant to
# back U-Boot verified boot later, which settles it: RSA-4096, PKCS#1 v1.5,
# SHA-256 (CONFIG_SIGALG_RAWRSA, default y under CONFIG_SIGNED_IMAGES).

SOLAR_SWU_PRIVATE_KEY ??= ""
SOLAR_SWU_PUBLIC_KEY ??= ""

# Dev-key dir: both recipes that inherit this class sit exactly two levels
# below the layer root (fw/recipes-<x>/<y>/), so THISDIR/../.. resolves.
SOLAR_DEV_KEY_DIR = "${@os.path.normpath(os.path.join(d.getVar('THISDIR'), '..', '..', 'files', 'keys', 'dev'))}"

SWUPDATE_SIGNING = "RSA"
# swupdate-common checks os.path.exists() at TASK time, so the private key
# must be materialized under WORKDIR before do_swuimage runs - see the task
# below. (It is deliberately not read straight from the host path: keeping
# one canonical location works identically for the content and path modes.)
SWUPDATE_PRIVATE_KEY = "${WORKDIR}/swu-signing/private.pem"

def solar_swu_get_key(d, var, devfile):
    """Resolve SOLAR_SWU_*_KEY to PEM text: content | absolute path | dev key."""
    import os
    val = (d.getVar(var) or "").strip()
    if val.startswith("-----BEGIN"):
        if not val.endswith("\n"):
            val += "\n"
        return val
    if val.startswith("/"):
        if not os.path.isfile(val):
            bb.fatal("%s names a file that does not exist: %s" % (var, val))
        with open(val) as f:
            return f.read()
    if val:
        bb.fatal("%s is neither PEM content (starts with '-----BEGIN') nor an "
                 "absolute host path. Fix the environment." % var)
    devpath = os.path.normpath(os.path.join(
        d.getVar("THISDIR"), "..", "..", "files", "keys", "dev", devfile))
    bb.warn("%s is empty: using the COMMITTED THROWAWAY DEV key (%s). "
            "Artifacts signed with it must never leave the lab." % (var, devpath))
    with open(devpath) as f:
        return f.read()

python do_swu_signing_keys() {
    import os
    outdir = os.path.join(d.getVar("WORKDIR"), "swu-signing")
    os.makedirs(outdir, exist_ok=True)

    priv = solar_swu_get_key(d, "SOLAR_SWU_PRIVATE_KEY", "private.pem")
    path = os.path.join(outdir, "private.pem")
    with open(path, "w") as f:
        f.write(priv)
    os.chmod(path, 0o600)

    pub = solar_swu_get_key(d, "SOLAR_SWU_PUBLIC_KEY", "public.pem")
    with open(os.path.join(outdir, "public.pem"), "w") as f:
        f.write(pub)
}

# Register the task; the before-ordering goes to whichever consuming task
# this recipe actually has: do_swuimage (the .swu recipe signs with the
# private key) or do_install (the agent bakes the public key into the
# rootfs). A hardcoded `addtask ... before do_swuimage` would be a
# build-dependency error in recipes that lack that task.
addtask swu_signing_keys
python () {
    import bb.build
    for before in ("do_swuimage", "do_install"):
        if d.getVarFlag(before, "task"):
            bb.build.addtask("do_swu_signing_keys", before, "", d)
            break
}

# The tasks above consume the env vars, but bitbake only re-runs tasks whose
# inputs changed - without vardeps, swapping keys would reuse stale sstate.
# do_swu_signing_keys needs it too: it reads the vars through a string-literal
# variable NAME (solar_swu_get_key(d, "SOLAR_SWU_...")), which vartable static
# analysis cannot see - without this vardeps a key rotation re-runs
# do_swuimage (vardeps below) but replays the STALE staged key here, silently
# re-signing with the old keypair while the image bakes the new one (hit
# 2026-09-28: dev-signed .swu vs real-key image, signature verify failure).
do_swu_signing_keys[vardeps] += "SOLAR_SWU_PRIVATE_KEY SOLAR_SWU_PUBLIC_KEY"
do_swuimage[vardeps] += "SOLAR_SWU_PRIVATE_KEY"
do_install[vardeps] += "SOLAR_SWU_PUBLIC_KEY"
