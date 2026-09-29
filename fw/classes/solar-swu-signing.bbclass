# SWUpdate signing plumbing for solar-ctl.
#
# Key handling mirrors the WiFi-credential policy (see fw/kas-rpi0.yml `env:`):
# the key material comes from the environment, never from a committed file -
#   SOLAR_SWU_PRIVATE_KEY  (REQUIRED)
#   SOLAR_SWU_PUBLIC_KEY   (optional: derived from the private key)
# and each value is EITHER the PEM content itself (starts with "-----BEGIN")
# OR an absolute path to a PEM file on the build host. There is deliberately
# NO committed fallback keypair: unset/invalid keys are a hard bb.fatal, so a
# build can never silently sign with (or bake) keys someone "helpfully" left
# in the repo (fw/tools/swu-keygen.sh makes a keypair; local = .zed/tasks.json
# env, CI = Actions secret SOLAR_SWU_PRIVATE_KEY - private key only).
#
# If SOLAR_SWU_PUBLIC_KEY is set it is CROSS-CHECKED against the private key
# (mismatch = fatal): the public key always provably belongs to the key that
# signs, which also kills the stale-pair class of bugs (real incident 2026-09-28:
# staged-PEM staleness produced dev-signed .swu vs real-key image).
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

SWUPDATE_SIGNING = "RSA"
# swupdate-common checks os.path.exists() at TASK time, so the private key
# must be materialized under WORKDIR before do_swuimage runs - see the task
# below. (It is deliberately not read straight from the host path: keeping
# one canonical location works identically for the content and path modes.)
SWUPDATE_PRIVATE_KEY = "${WORKDIR}/swu-signing/private.pem"

# openssl-native: the public key is derived/verified with `openssl rsa
# -pubout` in do_swu_signing_keys. solar-swu-agent does not otherwise pull
# openssl into its sysroot, so depend explicitly (swupdate-common gets it via
# SWUPDATE_SIGNING, but the task must not depend on which recipe inherits us).
do_swu_signing_keys[depends] += "openssl-native:do_populate_sysroot"

def solar_swu_get_key(d, var, required):
    """Resolve SOLAR_SWU_*_KEY to PEM text: content | absolute path | fatal.
    Returns None only for an unset optional variable."""
    import os
    val = (d.getVar(var) or "").strip()
    if not val:
        if not required:
            return None
        bb.fatal("%s is not set. There is no committed fallback key: run "
                 "fw/tools/swu-keygen.sh and wire the value in (PEM content "
                 "or absolute path) via .zed/tasks.json env locally, or the "
                 "SOLAR_SWU_PRIVATE_KEY Actions secret in CI." % var)
    if val.startswith("-----BEGIN"):
        if not val.endswith("\n"):
            val += "\n"
        return val
    if val.startswith("/"):
        if not os.path.isfile(val):
            bb.fatal("%s names a file that does not exist: %s" % (var, val))
        with open(val) as f:
            return f.read()
    bb.fatal("%s is neither PEM content (starts with '-----BEGIN') nor an "
             "absolute host path. Fix the environment." % var)

python do_swu_signing_keys() {
    import os, subprocess
    outdir = os.path.join(d.getVar("WORKDIR"), "swu-signing")
    os.makedirs(outdir, exist_ok=True)

    priv = solar_swu_get_key(d, "SOLAR_SWU_PRIVATE_KEY", True)
    path = os.path.join(outdir, "private.pem")
    with open(path, "w") as f:
        f.write(priv)
    os.chmod(path, 0o600)

    # Derive the public key from the private key (PATH includes
    # recipe-sysroot-native/bin - see the openssl-native depends above).
    r = subprocess.run(
        ["openssl", "rsa", "-in", path, "-pubout"],
        capture_output=True)
    if r.returncode != 0:
        bb.fatal("SOLAR_SWU_PRIVATE_KEY is not a usable RSA private key: "
                 "openssl rsa -pubout failed: %s" %
                 r.stderr.decode(errors="replace").strip())
    derived = r.stdout.decode()

    given = solar_swu_get_key(d, "SOLAR_SWU_PUBLIC_KEY", False)
    if given and given.strip() != derived.strip():
        bb.fatal("SOLAR_SWU_PUBLIC_KEY does not match SOLAR_SWU_PRIVATE_KEY "
                 "(openssl rsa -pubout mismatch). Rotate both together or "
                 "unset SOLAR_SWU_PUBLIC_KEY and let it be derived.")

    with open(os.path.join(outdir, "public.pem"), "w") as f:
        f.write(derived)
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
# PUBLIC_KEY alone is NOT enough for do_install: the staged public.pem is now
# derived from the private key, so the private key is its real input.
do_install[vardeps] += "SOLAR_SWU_PRIVATE_KEY SOLAR_SWU_PUBLIC_KEY"
